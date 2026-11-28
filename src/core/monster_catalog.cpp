#include "core/monster_catalog.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "core/damage.h"

namespace dnd {
namespace {

auto ParseInt(std::string_view text, int& value) -> bool {
  if (text.empty()) {
    return false;
  }
  int parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  value = parsed;
  return true;
}

auto ErrorAt(const ContentSection& section, int line, std::string_view message)
    -> std::string {
  return "строка " + std::to_string(line) + ": монстр «" + section.id +
         "»: " + std::string(message);
}

auto Require(const ContentSection& section, std::string_view key,
             std::string& error) -> const ContentEntry* {
  const ContentEntry* entry = section.Find(key);
  if (entry == nullptr) {
    error =
        ErrorAt(section, section.line, "нет поля «" + std::string(key) + "»");
  }
  return entry;
}

auto ReadInt(const ContentSection& section, std::string_view key, int& value,
             int minimum, int maximum, std::string& error) -> bool {
  const ContentEntry* entry = Require(section, key, error);
  if (entry == nullptr) {
    return false;
  }
  if (!ParseInt(entry->value, value) || value < minimum || value > maximum) {
    error =
        ErrorAt(section, entry->line,
                "поле «" + std::string(key) + "»: нужно целое " +
                    std::to_string(minimum) + ".." + std::to_string(maximum));
    return false;
  }
  return true;
}

auto ReadOptionalInt(const ContentSection& section, std::string_view key,
                     int& value, int minimum, int maximum, std::string& error)
    -> bool {
  const ContentEntry* entry = section.Find(key);
  if (entry == nullptr) {
    return true;
  }
  if (!ParseInt(entry->value, value) || value < minimum || value > maximum) {
    error =
        ErrorAt(section, entry->line,
                "поле «" + std::string(key) + "»: нужно целое " +
                    std::to_string(minimum) + ".." + std::to_string(maximum));
    return false;
  }
  return true;
}

auto ParseHitDie(std::string_view text, int& sides) -> bool {
  if (text.starts_with("d") || text.starts_with("D")) {
    text.remove_prefix(1);
  }
  int parsed = 0;
  if (!ParseInt(text, parsed)) {
    return false;
  }
  if (parsed != 4 && parsed != 6 && parsed != 8 && parsed != 10 &&
      parsed != 12 && parsed != 20) {
    return false;
  }
  sides = parsed;
  return true;
}

auto ReadDefenses(const ContentSection& section, std::string_view key,
                  std::uint32_t& mask, std::string& error) -> bool {
  const ContentEntry* entry = section.Find(key);
  if (entry == nullptr) {
    return true;
  }
  std::uint32_t parsed = 0;
  if (!ParseDamageTypeList(entry->value, parsed)) {
    error = ErrorAt(section, entry->line,
                    "поле «" + std::string(key) + "»: неизвестный тип урона «" +
                        entry->value + "»");
    return false;
  }
  mask = parsed;
  return true;
}

auto ParseMonster(const ContentSection& section)
    -> std::pair<std::optional<MonsterTemplate>, std::string> {
  std::string error;
  if (section.type != "monster") {
    return {std::nullopt, "строка " + std::to_string(section.line) +
                              ": неизвестный тип «" + section.type + "»"};
  }

  MonsterTemplate monster{.id = section.id};
  const ContentEntry* name = Require(section, "name", error);
  if (name == nullptr) {
    return {std::nullopt, std::move(error)};
  }
  if (name->value.empty()) {
    return {std::nullopt, ErrorAt(section, name->line, "пустое имя")};
  }
  monster.name = name->value;

  const ContentEntry* hit_die = Require(section, "hit_die", error);
  if (hit_die == nullptr) {
    return {std::nullopt, std::move(error)};
  }
  if (!ParseHitDie(hit_die->value, monster.hit_die)) {
    return {
        std::nullopt,
        ErrorAt(section, hit_die->line,
                "поле «hit_die»: нужна кость d4, d6, d8, d10, d12 или d20")};
  }

  const ContentEntry* damage = Require(section, "damage", error);
  if (damage == nullptr) {
    return {std::nullopt, std::move(error)};
  }
  DiceExpression expression;
  DamageType type = DamageType::kNone;
  if (!ParseDamageProfile(damage->value, expression, type)) {
    return {std::nullopt, ErrorAt(section, damage->line,
                                  "поле «damage»: некорректный урон «" +
                                      damage->value + "»")};
  }
  monster.damage = damage->value;

  if (!ReadInt(section, "ac", monster.ac, 1, 100, error) ||
      !ReadInt(section, "con", monster.con_modifier, -100, 100, error) ||
      !ReadInt(section, "hit_dice", monster.hit_dice_count, 1, 100, error) ||
      !ReadInt(section, "speed", monster.speed, 0, 100, error) ||
      !ReadInt(section, "attack", monster.attack_bonus, -100, 100, error) ||
      !ReadOptionalInt(section, "save", monster.save_bonus, -100, 100, error) ||
      !ReadOptionalInt(section, "range", monster.attack_range, 0, 100, error) ||
      !ReadDefenses(section, "resist", monster.resist, error) ||
      !ReadDefenses(section, "vulnerable", monster.vulnerable, error) ||
      !ReadDefenses(section, "immune", monster.immune, error)) {
    return {std::nullopt, std::move(error)};
  }
  if (const ContentEntry* notes = section.Find("notes"); notes != nullptr) {
    monster.notes = notes->value;
  }
  return {std::move(monster), {}};
}

}  // namespace

auto MonsterContentSchemas() -> std::span<const ContentSchema> {
  static constexpr std::array<std::string_view, 14> keys{
      "name", "ac",     "hit_die", "con",   "hit_dice", "speed",      "attack",
      "save", "damage", "notes",   "range", "resist",   "vulnerable", "immune",
  };
  static constexpr std::array<ContentSchema, 1> schemas{
      ContentSchema{.type = "monster", .keys = keys},
  };
  return schemas;
}

auto ParseMonsterCatalog(const ContentFile& file, std::string& error)
    -> std::optional<std::vector<MonsterTemplate>> {
  error.clear();
  std::vector<MonsterTemplate> monsters;
  for (const ContentSection& section : file.sections) {
    auto [monster, monster_error] = ParseMonster(section);
    if (!monster.has_value()) {
      error = std::move(monster_error);
      return std::nullopt;
    }
    const bool duplicate = std::ranges::any_of(
        monsters, [&monster](const MonsterTemplate& other) -> bool {
          return other.id == monster->id;
        });
    if (duplicate) {
      error = ErrorAt(section, section.line, "повтор id «" + section.id + "»");
      return std::nullopt;
    }
    monsters.push_back(std::move(*monster));
  }
  return monsters;
}

auto ParseMonsterContent(std::string_view text, std::string& error)
    -> std::optional<std::vector<MonsterTemplate>> {
  ContentFile file;
  if (!ParseContent(text, MonsterContentSchemas(), file, error)) {
    return std::nullopt;
  }
  return ParseMonsterCatalog(file, error);
}

}  // namespace dnd
