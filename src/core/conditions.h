#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace dnd {

struct Condition;

// Автоматическое состояние при нуле HP; его ставит и снимает сервер.
inline constexpr std::string_view kUnconscious = "без сознания";
inline constexpr std::string_view kConcentration = "концентрация";
inline constexpr std::string_view kDead = "мёртв";
// «Выведен из строя» и родственные состояния рвут концентрацию.
inline constexpr std::string_view kIncapacitated = "выведен из строя";
// Каждая запись «истощения» — один уровень (полная шкала — в P1).
inline constexpr std::string_view kExhaustion = "истощение";

// Время в D&D: раунд — 6 секунд, минута — 10 раундов. Длительности
// состояний хранятся в игровых секундах.
inline constexpr int kSecondsPerRound = 6;
inline constexpr int kSecondsPerMinute = 60;
inline constexpr int kSecondsPerHour = 60 * kSecondsPerMinute;
// Сколько действует состояние, включённое в панели K обычным нажатием.
inline constexpr int kDefaultConditionSeconds = kSecondsPerMinute;
// Предел длительности одного состояния и разового сдвига времени: 7 дней.
inline constexpr int kMaxConditionSeconds = 7 * 24 * kSecondsPerHour;
// Предел КС спасброска состояния.
inline constexpr int kMaxSaveDc = 30;

// Игровая длительность по-русски: «1 минута», «10 ходов», «2 часа».
// 0 — состояние держится до ручного снятия.
[[nodiscard]] auto FormatConditionDuration(int seconds) -> std::string;

// КС проверки концентрации: 10 или половина урона (вниз), что больше, но не
// выше 30 (SRD 5.2.1).
[[nodiscard]] auto ConcentrationDc(int damage) -> int;

// Недееспособность: «без сознания», «выведен из строя», «парализован»,
// «оглушён», «окаменел» и смерть. При таких состояниях концентрация
// снимается без спасброска.
[[nodiscard]] auto IsIncapacitatingCondition(std::string_view name) -> bool;

// Старые сохранения хранили длительность в ходах: перевести их в секунды.
auto ConvertLegacyConditionRounds(std::vector<Condition>& conditions) -> void;

// Стандартные метки состояний. Это подсказки для стола, а не автоматические
// модификаторы бросков.
[[nodiscard]] auto DefaultConditions() -> const std::vector<std::string>&;

}  // namespace dnd
