#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "core/damage.h"
#include "core/light.h"
#include "core/token.h"

namespace dnd {

// Автоматическое состояние при нуле HP; его ставит и снимает сервер.
inline constexpr std::string_view kUnconscious = "без сознания";
inline constexpr std::string_view kConcentration = "концентрация";
inline constexpr std::string_view kDead = "мёртв";
// «Выведен из строя» и родственные состояния рвут концентрацию.
inline constexpr std::string_view kIncapacitated = "выведен из строя";
// Стабильный персонаж на 0 HP не бросает спасброски от смерти и приходит в
// себя через 1d4 часа; срок держит само состояние (2024).
inline constexpr std::string_view kStable = "стабилен";
// Каждая запись «истощения» — один уровень, всего их до шести.
inline constexpr std::string_view kExhaustion = "истощение";
// Застигнутый врасплох бросает инициативу с помехой и теряет состояние
// после броска (2024).
inline constexpr std::string_view kSurprised = "застигнут врасплох";
// Ослеплённый персонаж не даёт обзора и не видит карту (core/visibility).
inline constexpr std::string_view kBlinded = "ослеплён";
// «Очарован» и «испуган» помнят источник (Condition::source_id): очарование
// запрещает атаковать очаровавшего, испуг — приближаться к источнику страха.
// «Оглох» пока без механики: проверок на слух в проекте нет.
inline constexpr std::string_view kCharmed = "очарован";
inline constexpr std::string_view kFrightened = "испуган";
inline constexpr std::string_view kDeafened = "оглох";
inline constexpr int kMaxExhaustionLevel = 6;

// Время в D&D: раунд — 6 секунд, минута — 10 раундов. Длительности
// состояний хранятся в игровых секундах.
inline constexpr int kSecondsPerRound = 6;
inline constexpr int kSecondsPerMinute = 60;
inline constexpr int kSecondsPerHour = 60 * kSecondsPerMinute;
// Сколько действует состояние, включённое в панели K обычным нажатием.
inline constexpr int kDefaultConditionSeconds = kSecondsPerMinute;
// Предел длительности одного состояния и разового сдвига времени: 7 дней.
inline constexpr int kMaxConditionSeconds = 7 * 24 * kSecondsPerHour;
// Следующий длинный отдых — не раньше чем через 16 часов после начала
// предыдущего (2024).
inline constexpr int kLongRestCooldownSeconds = 16 * kSecondsPerHour;
// Потолок игровых часов: больше 30 лет за кампанию не нужно, а переполнение
// int не должно ломать сравнение времени отдыха.
inline constexpr int kMaxGameTimeSeconds = 1000000000;
// Предел КС спасброска состояния.
inline constexpr int kMaxSaveDc = 30;
// Предел имени состояния в байтах UTF-8.
inline constexpr std::size_t kMaxConditionNameBytes = 32;

[[nodiscard]] auto HasCondition(const Token& token, std::string_view name)
    -> bool;

// Источник состояния (например, очаровавшего или источника страха).
// 0 — источник не записан.
[[nodiscard]] auto ConditionSource(const Token& token, std::string_view name)
    -> TokenId;

// Очарованный не атакует очаровавшего (SRD 5.2.1).
[[nodiscard]] auto IsCharmedAgainst(const Token& attacker, TokenId target)
    -> bool;

// Источник страха; 0 — состояние не «испуган» или источник не записан.
[[nodiscard]] auto FrightenedSource(const Token& token) -> TokenId;

// Шаг приближает к источнику (клетки считаются по Чебышёву).
[[nodiscard]] auto IsApproaching(Cell from, Cell to, Cell source) -> bool;

// Игровая длительность по-русски: «1 минута», «10 ходов», «2 часа».
// 0 — состояние держится до ручного снятия.
[[nodiscard]] auto FormatConditionDuration(int seconds) -> std::string;

// КС проверки концентрации: 10 или половина урона (вниз), что больше, но не
// выше 30 (SRD 5.2.1).
[[nodiscard]] auto ConcentrationDc(int damage) -> int;

// Недееспособность токена: 0 HP, «без сознания», «выведен из строя»,
// «парализован», «оглушён», «окаменел» и смерть.
[[nodiscard]] auto IsIncapacitatingCondition(std::string_view name) -> bool;

[[nodiscard]] auto IsIncapacitated(const Token& token) -> bool;

// Палитра состояний 2024 для панели K (истощение — одна запись, уровень
// считается по числу записей).
[[nodiscard]] auto DefaultConditions() -> const std::vector<std::string>&;

// Уровень истощения 1–6: каждая запись «истощение» — один уровень.
[[nodiscard]] auto ExhaustionLevel(const std::vector<Condition>& conditions)
    -> int;
[[nodiscard]] auto ExhaustionLevel(const Token& token) -> int;
// −2 ко всем d20-проверкам за уровень (атаки, инициатива, спасброски).
[[nodiscard]] auto ExhaustionPenalty(const Token& token) -> int;
// −5 футов скорости за уровень: на сетке клетка за уровень.
[[nodiscard]] auto ExhaustionSpeedPenaltyCells(const Token& token) -> int;
// Захвачен, опутан, парализован, оглушён, окаменел, без сознания или мёртв:
// скорость равна нулю.
[[nodiscard]] auto IsSpeedZeroed(const Token& token) -> bool;
// Скорость в клетках с учётом состояний и истощения; может быть 0.
[[nodiscard]] auto EffectiveSpeedCells(const Token& token) -> int;

// Помеха и преимущество одной атаки по состояниям 2024. Источники
// складываются; если есть и то и другое, бросок идёт без модификатора.
struct AttackRollModes {
  bool advantage = false;
  bool disadvantage = false;

  [[nodiscard]] auto Net() const -> int {
    return (advantage ? 1 : 0) - (disadvantage ? 1 : 0);
  }
};

// distance_cells — расстояние между клетками в 5-футовых клетках.
// attack_range_cells > 0 — дальнобойная атака с такой обычной дистанцией:
// за дистанцией помеха, а в 5 футах от другого противника (threatened) —
// тоже помеха. can_see_target / target_can_see_attacker — взгляд через
// сильное затемнение и тьму (CanSeeForAttack): не видно цель — помеха, цель
// не видит атакующего — преимущество; вместе они гасятся.
// frightened_applies — источник страха в поле зрения (или не записан):
// «испуган» даёт помеху, только пока источник виден (2024).
[[nodiscard]] auto AttackRollModesFor(const Token& attacker,
                                      const Token& target, int distance_cells,
                                      int attack_range_cells, bool threatened,
                                      bool can_see_target,
                                      bool target_can_see_attacker,
                                      bool frightened_applies = true)
    -> AttackRollModes;

// Видит ли наблюдатель клетку для атаки: слепота, сильное затемнение и тьма
// без тёмного зрения закрывают взгляд (SRD 5.2.1). Слабое затемнение и
// тусклый свет взгляду не мешают.
[[nodiscard]] auto CanSeeForAttack(const GridMap& map, const LightField& light,
                                   const Token& observer, Cell cell) -> bool;

// Полный взгляд на клетку: стены (core/visibility) плюс свет, затемнение и
// тёмное зрение. Нужен правилам вроде источника страха, где мало одного
// покрытия атаки.
[[nodiscard]] auto CanSeeCell(const GridMap& map, const LightField& light,
                              const Token& observer, Cell cell) -> bool;

// Инициатива 2024: невидимый бросает с преимуществом, недееспособный и
// застигнутый врасплох — с помехой; преимущество и помеха гасятся.
[[nodiscard]] auto InitiativeRollModesFor(const Token& token)
    -> AttackRollModes;

// Парализованный и лежащий без сознания получают критические попадания от
// атак в пределах 5 футов (SRD 5.2.1).
[[nodiscard]] auto IsAutoCriticalOnHit(const Token& target, int distance_cells)
    -> bool;

// Защита цели с учётом состояний: «окаменел» даёт сопротивление всему урону
// и иммунитет к яду.
[[nodiscard]] auto EffectiveDamageProfile(const Token& target) -> DamageProfile;

[[nodiscard]] auto IsSurprised(const Token& token) -> bool;

}  // namespace dnd
