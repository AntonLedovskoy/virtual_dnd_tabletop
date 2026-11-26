#pragma once

#include <cstdint>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "core/command.h"
#include "core/event.h"
#include "core/game_state.h"
#include "core/snapshot.h"
#include "core/token.h"
#include "core/visibility.h"

namespace dnd {

struct CommandOutcome {
  bool accepted = false;
  std::string reject_reason;
  std::vector<Event> events;
};

// Авторитетная сессия: владеет миром, проверяет команды с учётом владения
// токенами, бросает кубы и хранит срез видимости локального игрока.
// Сетевая игра запускает тот же класс на хосте.
class GameServer {
 public:
  // Обзор партии дают только токены персонажей (kPlayer), не NPC и не монстры.
  GameServer(const GameSnapshot& snapshot, int sight_radius,
             std::uint32_t rng_seed = std::random_device{}());

  // Заменяет мир (загрузка/сохранение) и пересчитывает видимость.
  auto Reset(GameSnapshot snapshot) -> void;
  // Новая кампания: игроки снова выбирают персонажа один раз. Смену сцены и
  // отмену это не трогает, иначе выбор можно было бы повторить.
  auto ClearClaimHistory() -> void { claimed_players_.clear(); }

  // Применяет команду. `actor` — имя игрока из handshake, `can_control_all`
  // истинно для DM. Токены с непустым owner может менять только владелец;
  // DM может менять всё.
  [[nodiscard]] auto HandleCommand(const Command& command,
                                   const std::string& actor,
                                   bool can_control_all) -> CommandOutcome;

  [[nodiscard]] auto State() const -> const GameState& { return state_; }
  // Мир вместе с туманом игроков: так сцена попадает в файл и в историю отмены.
  [[nodiscard]] auto Snapshot() const -> GameSnapshot;
  // Вид игрока: только клетки и токены в его собственном поле зрения.
  [[nodiscard]] auto ClientSnapshot(std::string_view actor) const
      -> GameSnapshot;
  // Вид DM: весь мир; туман ему не нужен, поэтому в снимке его нет.
  [[nodiscard]] auto DmSnapshot() const -> GameSnapshot {
    return state_.Snapshot();
  }
  [[nodiscard]] auto FogState(std::string_view actor) const -> FogSnapshot;
  // DM видит все клетки, поэтому туману нечего скрывать.
  [[nodiscard]] auto FullFogState() const -> FogSnapshot;

  // Можно ли отправлять это событие конкретному игроку.
  [[nodiscard]] auto EventVisibleTo(const Event& event,
                                    std::string_view actor) const -> bool;

  // Инкрементальная синхронизация вида: токены, которые вошли в поле зрения
  // или ушли из него, и изменившиеся клетки тумана. Слепок «уже отправлено»
  // обновляется здесь же, поэтому вызывать нужно один раз на отправку.
  [[nodiscard]] auto TakeViewDelta(std::string_view actor)
      -> std::vector<Event>;
  // Игроку отправлен полный вид (вход, смена мира): дельта снова от нуля.
  auto MarkViewSent(std::string_view actor) -> void;
  // Видит ли игрок клетку: собственный туман или ручное открытие DM.
  [[nodiscard]] auto IsVisibleTo(std::string_view actor, Cell cell) const
      -> bool;
  // Разведана ли клетка (видели раньше), даже если сейчас она в тумане.
  [[nodiscard]] auto IsExploredTo(std::string_view actor, Cell cell) const
      -> bool;

 private:
  [[nodiscard]] static auto CanControl(const Token& token,
                                       const std::string& actor,
                                       bool can_control_all) -> bool;

  [[nodiscard]] auto HandleMove(const MoveToken& command,
                                const std::string& actor, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] static auto HandleSay(const Say& command,
                                      const std::string& actor)
      -> CommandOutcome;
  [[nodiscard]] auto HandleRoll(const RollDice& command,
                                const std::string& actor) -> CommandOutcome;
  [[nodiscard]] auto HandleRollExpression(const RollExpression& command,
                                          const std::string& actor)
      -> CommandOutcome;
  [[nodiscard]] auto HandleAddToken(const AddToken& command,
                                    bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleRemoveToken(const RemoveToken& command,
                                       bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleSetHp(const SetTokenHp& command,
                                 const std::string& actor, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleDamage(const DamageToken& command,
                                  const std::string& actor,
                                  bool can_control_all) -> CommandOutcome;
  // Атака: сравнение с КД цели и урон при попадании. Цель должна быть видна
  // атакующему игроку; DM бьёт любого. Игрок бьёт только в свой ход и тратит
  // действие; вне своего хода — реакция по соседней цели.
  [[nodiscard]] auto HandleAttack(const AttackToken& command,
                                  const std::string& actor,
                                  bool can_control_all) -> CommandOutcome;
  // Стандартное действие хода: проверяет ход, недееспособность и остаток
  // действия, включает эффект (рывок, отход, уклонение) и пишет строку в лог.
  [[nodiscard]] auto HandleUseTurnAction(const UseTurnAction& command,
                                         const std::string& actor,
                                         bool can_control_all)
      -> CommandOutcome;
  // Причина, по которой токен не может действовать сейчас; nullptr — можно.
  // DM не ограничен, вне боя ход не проверяется.
  [[nodiscard]] auto TurnRejectReason(const Token& token,
                                      bool can_control_all) const -> const
      char*;
  // Наносит урон и добавляет события токена, лога и проверки концентрации.
  // critical — критическое попадание (урон по лежачему даёт два провала).
  // Тип урона включает сопротивление, уязвимость и иммунитет цели.
  auto DamageAndLog(TokenId id, int amount, bool critical,
                    DamageType damage_type, const std::string& actor,
                    std::vector<Event>& events) -> void;
  // Срабатывание профиля урона клетки (вход, начало/конец хода). Повторно в
  // том же ходу профиль не бьёт; спасбросок с КС делит урон пополам.
  auto TriggerHazards(TokenId id, std::uint8_t trigger,
                      const std::string& actor, std::vector<Event>& events)
      -> void;
  // Течение в клетке токена: проверка Силы (Атлетика), при провале сдвиг по
  // течению без траты движения. Столкновения и границы карты останавливают.
  auto TryCurrent(TokenId id, const std::string& actor,
                  std::vector<Event>& events) -> void;
  [[nodiscard]] auto HandleHeal(const HealToken& command,
                                const std::string& actor, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleRest(const RestParty& command, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleFall(const FallToken& command, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleAdvanceTime(const AdvanceTime& command,
                                       bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleAoeDamage(const AoeDamage& command,
                                     bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleDeathSave(const DeathSave& command,
                                     const std::string& actor,
                                     bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleSetInitiative(const SetInitiative& command,
                                         const std::string& actor,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleClaimToken(const ClaimToken& command,
                                      const std::string& actor)
      -> CommandOutcome;
  [[nodiscard]] auto HandleReleaseToken(const ReleaseToken& command,
                                        bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleSetTokenOwner(const SetTokenOwner& command,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleRenameToken(const RenameToken& command,
                                       bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleSetTokenMaxHp(const SetTokenMaxHp& command,
                                         const std::string& actor,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleSetTokenColor(const SetTokenColor& command,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleSetTokenPortrait(const SetTokenPortrait& command,
                                            const std::string& actor,
                                            bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleSetTokenLight(const SetTokenLight& command,
                                         const std::string& actor,
                                         bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleUpdateTokenSheet(const UpdateTokenSheet& command,
                                            bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleUpdateTokenCombat(const UpdateTokenCombat& command,
                                             bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleStartEncounter(bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleEndEncounter(bool can_control_all) -> CommandOutcome;
  // DM правит состав боя: вступление с броском инициативы или выход.
  [[nodiscard]] auto HandleSetEncounterComposition(
      const SetEncounterComposition& command, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleSetAoe(const SetAoe& command, bool can_control_all)
      -> CommandOutcome;
  // Метка «сюда»: любой участник, клетка проверяется по карте.
  [[nodiscard]] auto HandlePing(const PingAt& command, const std::string& actor)
      -> CommandOutcome;
  // Подложку карты меняет только DM: это общий вид стола.
  [[nodiscard]] auto HandleSetMapBackground(const SetMapBackground& command,
                                            bool can_control_all)
      -> CommandOutcome;
  // Открытие областей столу: ручной инструмент DM.
  [[nodiscard]] auto HandleReveal(const RevealArea& command,
                                  bool can_control_all) -> CommandOutcome;
  [[nodiscard]] auto HandleHide(const HideArea& command, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleClearRevealed(bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleAwardXp(const AwardXp& command, bool can_control_all)
      -> CommandOutcome;
  [[nodiscard]] auto HandleNextTurn(bool can_control_all) -> CommandOutcome;

  [[nodiscard]] auto FindToken(TokenId id) const -> const Token*;
  // Виден ли стол клетке: общий обзор партии и ручные открытия DM.
  [[nodiscard]] auto IsPartyVisible(Cell cell) const -> bool;
  // Состав боя по умолчанию: все персонажи и замеченные ими не-персонажи
  // (в поле зрения партии, живые). Без персонажей — все токены.
  [[nodiscard]] auto DefaultParticipants() const -> std::vector<TokenId>;
  // Бросок инициативы d20 + бонус − истощение; «застигнут врасплох» даёт
  // помеху и снимается после броска.
  auto RollInitiative(TokenId id) -> int;
  // Добавляет токена в идущий бой, если его там нет: бросок, вставка в конец
  // раунда, событие порядка и строка «X вступает в бой». Возвращает true,
  // если токен действительно вступил.
  auto JoinEncounterIfNeeded(TokenId id, std::vector<Event>& events) -> bool;
  [[nodiscard]] auto PlayerFog(std::string_view actor) const -> const FogOfWar*;
  // Токен виден игроку: клетка в его тумане и не в сильном затемнении.
  [[nodiscard]] auto IsTokenVisibleTo(std::string_view actor,
                                      const Token& token) const -> bool;
  // Туман сцены: переживает смену мира, карту другой формы не переносит.
  auto RestoreFog(const std::vector<FogRecord>& records) -> void;
  [[nodiscard]] auto FogRecords() const -> std::vector<FogRecord>;
  auto RecomputeFog() -> void;

  GameState state_;
  // Туман на каждого владельца персонажа; ключ — PlayerNameKey игрока,
  // поэтому регистр в /owner и в имени подключившегося не важен.
  std::map<std::string, FogOfWar, std::less<>> fogs_;
  // Что хост уже отправил игроку: видимые токены и его туман. По этому
  // слепку считается дельта, поэтому полный снимок ходит только при входе
  // и смене мира.
  struct SentView {
    std::vector<TokenId> tokens;
    FogSnapshot fog;
  };
  std::map<std::string, SentView, std::less<>> sent_views_;
  // Кто уже выбирал персонажа в этой кампании: выбор даётся один раз, потом
  // владельцев назначает только DM через /owner.
  std::set<std::string, std::less<>> claimed_players_;
  // У кого сейчас есть персонаж. Игрок без персонажа смотрит общим обзором
  // партии, даже если в сцене остался его туман от прошлой партии.
  std::set<std::string, std::less<>> character_owners_;
  // Обзор партии: его получает игрок, пока не выбрал персонажа.
  FogOfWar party_fog_;
  int sight_radius_;
  std::mt19937 rng_;
};

}  // namespace dnd
