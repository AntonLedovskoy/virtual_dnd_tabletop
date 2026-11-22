#pragma once

#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include "core/command.h"
#include "core/encounter.h"
#include "core/event.h"
#include "core/grid_map.h"
#include "core/snapshot.h"
#include "core/token.h"

namespace dnd {

// Итог отдыха одного токена: новый вид токена и сколько он потратил и
// восстановил, чтобы сервер записал подробную строку в лог.
struct RestChange {
  Token token;
  int dice_spent = 0;
  int hp_restored = 0;
  // Сколько уровней истощения снял длинный отдых.
  int exhaustion_removed = 0;
};

// Авторитетное состояние мира. В локальной игре работает в том же процессе
// рядом с клиентом; по сети живёт только на хосте.
class GameState {
 public:
  explicit GameState(GridMap map);
  explicit GameState(GameSnapshot snapshot);

  [[nodiscard]] auto Map() const -> const GridMap& { return map_; }
  [[nodiscard]] auto Tokens() const -> const std::vector<Token>& {
    return tokens_;
  }
  [[nodiscard]] auto Snapshot() const -> GameSnapshot;
  [[nodiscard]] auto TokenCopy(TokenId id) const -> std::optional<Token>;

  [[nodiscard]] auto AddToken(
      std::string name, Cell cell, std::string owner = {},
      TokenKind kind = TokenKind::kPlayer, int max_hp = 10,
      std::uint32_t color = 0, int ac = 10, int speed = 9,
      std::string notes = {}, int attack_bonus = 0, int save_bonus = 0,
      std::string damage = {}, int attack_range = 0, std::uint32_t resist = 0,
      std::uint32_t vulnerable = 0, std::uint32_t immune = 0) -> TokenId;

  // Применяет команду клиента; возвращает событие для рассылки, если команда
  // изменила мир.
  [[nodiscard]] auto ApplyCommand(const Command& command)
      -> std::optional<Event>;

  // Обновление вида на клиенте: двигать токены разрешено только событиями.
  auto ApplyEvent(const Event& event) -> void;

  auto RemoveToken(TokenId id) -> bool;
  [[nodiscard]] auto SetTokenHp(TokenId id, int hp) -> std::optional<Token>;
  // Временные HP принимают урон первыми; при нуле HP ставится «без сознания».
  // critical — критическое попадание: по лежачему оно даёт два провала
  // спасброска от смерти вместо одного. Урон, чей остаток после падения до
  // нуля не меньше максимума HP, убивает сразу (SRD 5.2.1, Instant Death).
  [[nodiscard]] auto DamageToken(TokenId id, int amount, bool critical = false)
      -> std::optional<Token>;
  // Лечение не поднимает выше максимума; при HP > 0 снимает «без сознания».
  [[nodiscard]] auto HealToken(TokenId id, int amount) -> std::optional<Token>;
  // Бросок спасброска от смерти при hp == 0; roll — уже выпавший d20.
  // Натуральная 1 даёт два провала, 20 возвращает 1 HP. Три успеха оставляют
  // персонажа стабильным, но всё ещё без сознания на нуле HP. penalty —
  // истощение: оно вычитается из результата, но не из натурального значения.
  [[nodiscard]] auto DeathSave(TokenId id, int roll, int penalty = 0)
      -> std::optional<Token>;
  // Отдых по правилам 2024. Короткий (1 час) тратит кости хитов на HP:
  // каждая даёт бросок d{hit_die} плюс модификатор Телосложения (не меньше
  // единицы), пока не восстановлен максимум или не кончился запас. Длинный
  // (8 часов) возвращает HP и кости хитов и снимает 1 уровень истощения.
  // Отдых даёт эффект только персонажу хотя бы с 1 HP: мёртвый и лежачий на
  // нуле не встаёт. Отдых двигает игровое время, поэтому срочные состояния
  // истекают. Возвращает изменившихся персонажей с подробностями для лога.
  [[nodiscard]] auto RestParty(bool long_rest, std::mt19937& rng)
      -> std::vector<RestChange>;
  [[nodiscard]] auto SetInitiative(TokenId id, int value)
      -> std::optional<Token>;
  [[nodiscard]] auto SetTokenOwner(TokenId id, std::string owner)
      -> std::optional<Token>;
  [[nodiscard]] auto RenameToken(TokenId id, std::string name)
      -> std::optional<Token>;
  [[nodiscard]] auto SetTokenMaxHp(TokenId id, int max_hp)
      -> std::optional<Token>;
  // Начисляет опыт, не выше kMaxXp; уровень вычисляется по опыту. При
  // повышении уровня персонаж получает HP по кости хитов и модификатору
  // Телосложения, а на смене рубежа растут бонусы атаки и спасброска.
  [[nodiscard]] auto AddXp(TokenId id, int amount) -> std::optional<Token>;
  [[nodiscard]] auto SetTokenColor(TokenId id, std::uint32_t color)
      -> std::optional<Token>;
  // Пустой portrait снимает картинку и возвращает цвет с инициалами.
  [[nodiscard]] auto SetTokenPortrait(TokenId id, std::string portrait)
      -> std::optional<Token>;
  [[nodiscard]] auto UpdateTokenSheet(
      TokenId id, int ac, int speed, std::string notes, int attack_bonus,
      int save_bonus, int initiative_bonus, std::string damage, int temp_hp,
      int hit_die, int con_modifier, int attack_range = 0,
      std::uint32_t resist = 0, std::uint32_t vulnerable = 0,
      std::uint32_t immune = 0) -> std::optional<Token>;
  [[nodiscard]] auto UpdateTokenCombat(TokenId id,
                                       std::vector<Condition> conditions)
      -> std::optional<Token>;
  [[nodiscard]] auto RemoveTokenCondition(TokenId id, std::string_view name)
      -> std::optional<Token>;

  // Добавляет к израсходованному запасу движения токена steps клеток и
  // запоминает чётность диагоналей после хода.
  [[nodiscard]] auto SpendMovement(TokenId id, int steps, int diagonal_parity)
      -> std::optional<Token>;
  // Тратит действие или реакцию токена; возвращает обновлённый токен.
  // Проверкой хода и недееспособности занимается сервер.
  [[nodiscard]] auto SpendAction(TokenId id) -> std::optional<Token>;
  [[nodiscard]] auto SpendReaction(TokenId id) -> std::optional<Token>;
  // Действие хода: отмечает действие потраченным и включает эффект приёма.
  [[nodiscard]] auto UseTurnAction(TokenId id, TurnActionKind kind)
      -> std::optional<Token>;

  // Боевой порядок.
  [[nodiscard]] auto EncounterState() const -> const Encounter& {
    return encounter_;
  }
  [[nodiscard]] auto ActiveTokenId() const -> TokenId;
  // Начинает бой из списка участников: кто не попал в него, в порядке хода
  // не появляется. Пустой список означает «все токены» (старое поведение и
  // прямые тесты GameState); сервер сам решает состав по видимости.
  [[nodiscard]] auto StartEncounter(std::vector<TokenId> participants = {})
      -> bool;
  auto EndEncounter() -> void;
  // Вступление в идущий бой: токен встаёт в конец раунда, текущий ход не
  // сдвигается. Повторное вступление и пустой бой отклоняются.
  [[nodiscard]] auto JoinEncounter(TokenId id) -> bool;
  // Выводит токен из идущего боя. Если уходил текущий, ход переходит к
  // следующему в порядке; если порядок опустел, бой завершается.
  [[nodiscard]] auto LeaveEncounter(TokenId id) -> bool;
  [[nodiscard]] auto IsEncounterParticipant(TokenId id) const -> bool;
  [[nodiscard]] auto NextTurn() -> bool;
  // Прокручивает игровое время вне боя: срочные состояния всех токенов
  // уменьшаются, истёкшие снимаются. Возвращает изменённые токены.
  [[nodiscard]] auto AdvanceTime(int seconds) -> std::vector<Token>;

  // Шаблон области, который DM показывает столу.
  [[nodiscard]] auto Aoe() const -> const AoeTemplate& { return aoe_; }
  auto SetAoe(AoeTemplate aoe) -> bool;

  // Подложка карты: имя файла и параметры отображения.
  [[nodiscard]] auto Background() const -> const MapBackground& {
    return background_;
  }
  auto SetBackground(MapBackground background) -> bool;

  // Клетки, которые DM открыл столу вручную. Circle includes center.
  // Хранятся отсортированными по (y, x): поиск двоичный, а области
  // сливаются за один проход.
  [[nodiscard]] auto Revealed() const -> const std::vector<Cell>& {
    return revealed_;
  }
  [[nodiscard]] auto IsRevealed(Cell cell) const -> bool;
  // Возвращает true, если список открытых клеток изменился.
  auto RevealArea(Cell center, int radius) -> bool;
  auto HideArea(Cell center, int radius) -> bool;
  auto ClearRevealed() -> bool;

 private:
  auto FindToken(TokenId id) -> Token*;
  [[nodiscard]] auto FindToken(TokenId id) const -> const Token*;
  auto RebuildOrder(TokenId keep) -> void;
  // Начало хода: полный запас движения, доступные действие, бонусное действие
  // и реакция, снятые эффекты рывка, отхода и уклонения.
  static auto ResetTurnEconomy(Token& token) -> void;

  GridMap map_;
  std::vector<Token> tokens_;
  TokenId next_token_id_ = 1;
  Encounter encounter_;
  AoeTemplate aoe_;
  MapBackground background_;
  std::vector<Cell> revealed_;
};

}  // namespace dnd
