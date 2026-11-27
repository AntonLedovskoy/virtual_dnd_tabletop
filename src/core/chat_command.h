#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/damage.h"
#include "core/skills.h"

namespace dnd {

// Что игрок ввёл в строке чата. Всё, что не похоже на известную команду,
// остаётся обычным сообщением (kSay).
enum class ChatCommandKind : std::uint8_t {
  kSay,
  kRoll,
  kClaim,
  kPing,
  kMapOff,
  kMapOpacity,
  kMapScale,
  kMapImage,
  kKick,
  kSceneList,
  kSceneNew,
  kSceneRename,
  kSceneSwitch,
  kAoe,
  kOwner,
  kRename,
  kPortrait,
  kMaxHp,
  kDamage,
  kHeal,
  kDeathSave,
  kStabilize,
  kRest,
  kTime,
  kFall,
  kReveal,
  kUnreveal,
  kRevealOff,
  kXp,
  kLight,
  kCheck,
};

// Причина отказа в разборе. Русские подсказки живут в src/app/strings.h:
// ядро знает только, что именно не так, и не тянет тексты интерфейса.
enum class ChatCommandError : std::uint8_t {
  kNone,
  kBadRoll,
  kBadKick,
  kBadRename,
  kBadMaxHp,
  kBadDamage,
  kBadAoe,
  kBadPortrait,
  kBadMap,
  kBadReveal,
  kBadXp,
  kBadTime,
  kBadRest,
  kBadFall,
  kBadLight,
  kBadCheck,
};

// Разобранная строка чата. При error != kNone команду отправлять нельзя,
// а kind не значим: приложение показывает подсказку по error.
struct ChatCommand {
  ChatCommandKind kind = ChatCommandKind::kSay;
  ChatCommandError error = ChatCommandError::kNone;
  // kSay — текст сообщения, kRoll — выражение, kKick/kRename/kOwner — имя,
  // kSceneSwitch/kSceneRename — имя или «СТАРОЕ НОВОЕ», kMapImage — файл.
  std::string text;
  // /maxhp N, /damage N, /heal N, /map opacity N, /map scale N, /time N,
  // /rest N (сколько костей хитов тратить), /fall N (высота в футах).
  int value = 0;
  // Сложность спасброска из /aoe.
  int save_dc = 0;
  // Тип урона из /damage N ТИП и /aoe ВЫРАЖЕНИЕ КС ТИП.
  DamageType damage_type = DamageType::kNone;
  bool long_rest = false;
  // /xp N all — опыт всей партии.
  bool party = false;
  // /light: радиусы яркого и тусклого света в метрах; 0 — свет выключен.
  int light_bright_meters = 0;
  int light_dim_meters = 0;
  // /check НАВЫК [КС]: навык проверки; КС лежит в save_dc.
  Skill skill = Skill::kPerception;
};

// Разбирает строку чата, не обращаясь к миру: роль (DM или игрок), выбранный
// токен и список сцен проверяет приложение.
auto ParseChatCommand(std::string_view text) -> ChatCommand;

}  // namespace dnd
