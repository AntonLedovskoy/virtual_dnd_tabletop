#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "core/dice.h"

namespace dnd {

// Типы урона SRD 5.2.1. kNone — урон без типа: сопротивление и уязвимость к
// нему не применяются.
enum class DamageType : std::uint8_t {
  kNone = 0,
  kBludgeoning = 1,
  kPiercing = 2,
  kSlashing = 3,
  kAcid = 4,
  kCold = 5,
  kFire = 6,
  kForce = 7,
  kLightning = 8,
  kNecrotic = 9,
  kPoison = 10,
  kPsychic = 11,
  kRadiant = 12,
  kThunder = 13,
};

inline constexpr int kDamageTypeCount = 14;
// Все типы, кроме kNone: биты 1..13.
inline constexpr std::uint32_t kAllDamageTypes = (1U << 14U) - 2U;

[[nodiscard]] constexpr auto DamageTypeBit(DamageType type) -> std::uint32_t {
  return std::uint32_t{1} << static_cast<std::uint32_t>(type);
}

[[nodiscard]] constexpr auto IsValidDamageTypeBit(std::uint32_t mask) -> bool {
  return (mask & ~kAllDamageTypes) == 0;
}

// Защита токена от урона: битовые маски типов. Сопротивление делит урон
// вдвое (вниз), уязвимость удваивает, иммунитет обнуляет; если есть и
// сопротивление, и уязвимость, сначала делится, потом удваивается (SRD).
struct DamageProfile {
  std::uint32_t resist = 0;
  std::uint32_t vulnerable = 0;
  std::uint32_t immune = 0;
};

// Русское имя типа для листа, лога и чата; пустая строка у kNone.
[[nodiscard]] auto DamageTypeName(DamageType type) -> std::string_view;

// Понимает русские имена («огонь», «дробящий») и английские названия.
[[nodiscard]] auto ParseDamageType(std::string_view text)
    -> std::optional<DamageType>;

// Список через запятую, точку с запятой или пробел; пустая строка — 0.
// Неизвестное имя — false, маска не меняется.
[[nodiscard]] auto ParseDamageTypeList(std::string_view text,
                                       std::uint32_t& mask) -> bool;

// Обратное преобразование для листа: «огонь, холод».
[[nodiscard]] auto FormatDamageTypeList(std::uint32_t mask) -> std::string;

// Урон после сопротивления, уязвимости и иммунитета (SRD 5.2.1).
[[nodiscard]] auto AdjustedDamage(int amount, DamageType type,
                                  const DamageProfile& profile) -> int;

// Урон из листа: «1d8+2 огонь» — выражение и необязательный тип в конце.
[[nodiscard]] auto ParseDamageProfile(std::string_view text,
                                      DiceExpression& expression,
                                      DamageType& type) -> bool;

// Может ли текст быть началом урона из листа: выражение или выражение с
// недопечатанным типом («1d8+2 ого»). Для посимвольного ввода.
[[nodiscard]] auto IsDamageProfilePrefix(std::string_view text) -> bool;

}  // namespace dnd
