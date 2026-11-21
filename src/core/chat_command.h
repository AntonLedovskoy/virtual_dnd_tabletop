#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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
  kRest,
  kReveal,
  kRevealOff,
  kXp,
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
};

// Разобранная строка чата. При error != kNone команду отправлять нельзя,
// а kind не значим: приложение показывает подсказку по error.
struct ChatCommand {
  ChatCommandKind kind = ChatCommandKind::kSay;
  ChatCommandError error = ChatCommandError::kNone;
  // kSay — текст сообщения, kRoll — выражение, kKick/kRename/kOwner — имя,
  // kSceneSwitch/kSceneRename — имя или «СТАРОЕ НОВОЕ», kMapImage — файл.
  std::string text;
  // /maxhp N, /damage N, /heal N, /map opacity N, /map scale N.
  int value = 0;
  // Сложность спасброска из /aoe.
  int save_dc = 0;
  bool long_rest = false;
  // /xp N all — опыт всей партии.
  bool party = false;
};

// Разбирает строку чата, не обращаясь к миру: роль (DM или игрок), выбранный
// токен и список сцен проверяет приложение.
auto ParseChatCommand(std::string_view text) -> ChatCommand;

}  // namespace dnd
