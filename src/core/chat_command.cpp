#include "core/chat_command.h"

#include <charconv>
#include <cstddef>
#include <system_error>

#include "core/dice.h"
#include "core/image_name.h"
#include "core/map_background.h"

namespace dnd {
namespace {

// Команда без хвоста или команда с хвостом через пробел: «/map» и «/map off».
auto MatchesCommand(std::string_view text, std::string_view name) -> bool {
  return text == name || (text.size() > name.size() && text.starts_with(name) &&
                          text[name.size()] == ' ');
}

// Хвост после «команда »; пустая строка, если хвоста нет.
auto CommandArgs(std::string_view text, std::string_view name)
    -> std::string_view {
  return text.size() > name.size() ? text.substr(name.size() + 1)
                                   : std::string_view{};
}

// Целое число целиком, без пробелов; ноль и отрицательные не годятся.
auto ParsePositiveNumber(std::string_view text, int& value) -> bool {
  if (text.empty()) {
    return false;
  }
  int parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      parsed <= 0) {
    return false;
  }
  value = parsed;
  return true;
}

// Как ParsePositiveNumber, но принимает отрицательные: предел проверяется
// отдельно (у прозрачности и масштаба подложки он свой).
auto ParseSignedNumber(std::string_view text, int& value) -> bool {
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

}  // namespace

auto ParseChatCommand(std::string_view text) -> ChatCommand {
  ChatCommand command{.text = std::string(text)};
  if (text.empty() || text.front() != '/') {
    return command;
  }

  const auto reject = [&command](ChatCommandError error) -> ChatCommand {
    command.error = error;
    return command;
  };

  if (MatchesCommand(text, "/roll")) {
    command.kind = ChatCommandKind::kRoll;
    command.text = std::string(CommandArgs(text, "/roll"));
    DiceExpression expression;
    if (!ParseDiceExpression(command.text, expression)) {
      return reject(ChatCommandError::kBadRoll);
    }
    return command;
  }

  if (text == "/claim") {
    command.kind = ChatCommandKind::kClaim;
    return command;
  }

  if (text == "/ping") {
    command.kind = ChatCommandKind::kPing;
    return command;
  }

  if (MatchesCommand(text, "/map")) {
    const std::string_view args = CommandArgs(text, "/map");
    command.kind = ChatCommandKind::kMapImage;
    command.text = std::string(args);
    if (args == "off") {
      command.kind = ChatCommandKind::kMapOff;
      command.text.clear();
      return command;
    }
    int value = 0;
    if (args.starts_with("opacity ")) {
      command.kind = ChatCommandKind::kMapOpacity;
      if (!ParseSignedNumber(args.substr(8), value) ||
          value < kMinBackgroundOpacity || value > kMaxBackgroundOpacity) {
        return reject(ChatCommandError::kBadMap);
      }
      command.value = value;
      return command;
    }
    if (args.starts_with("scale ")) {
      command.kind = ChatCommandKind::kMapScale;
      if (!ParseSignedNumber(args.substr(6), value) ||
          value < kMinBackgroundScale || value > kMaxBackgroundScale) {
        return reject(ChatCommandError::kBadMap);
      }
      command.value = value;
      return command;
    }
    if (!IsSafeImageName(args)) {
      return reject(ChatCommandError::kBadMap);
    }
    return command;
  }

  if (MatchesCommand(text, "/kick")) {
    command.kind = ChatCommandKind::kKick;
    command.text = std::string(CommandArgs(text, "/kick"));
    if (command.text.empty()) {
      return reject(ChatCommandError::kBadKick);
    }
    return command;
  }

  if (MatchesCommand(text, "/scene")) {
    const std::string_view args = CommandArgs(text, "/scene");
    if (args.empty()) {
      command.kind = ChatCommandKind::kSceneList;
      command.text.clear();
    } else if (args == "new") {
      command.kind = ChatCommandKind::kSceneNew;
      command.text.clear();
    } else if (args == "rename" || args.starts_with("rename ")) {
      // Хвост разбирает ParseSceneRename: имя сцены может быть из двух слов.
      command.kind = ChatCommandKind::kSceneRename;
      command.text = std::string(args.substr(6));
    } else {
      command.kind = ChatCommandKind::kSceneSwitch;
      command.text = std::string(args);
    }
    return command;
  }

  if (MatchesCommand(text, "/aoe")) {
    command.kind = ChatCommandKind::kAoe;
    const std::string_view args = CommandArgs(text, "/aoe");
    const std::size_t space = args.rfind(' ');
    int save_dc = 0;
    DiceExpression expression;
    if (space == std::string_view::npos ||
        !ParseDiceExpression(args.substr(0, space), expression) ||
        !ParsePositiveNumber(args.substr(space + 1), save_dc) ||
        save_dc > 100) {
      return reject(ChatCommandError::kBadAoe);
    }
    command.text = std::string(args.substr(0, space));
    command.save_dc = save_dc;
    return command;
  }

  if (MatchesCommand(text, "/owner")) {
    // Пустой хвост — снять владельца, это допустимо.
    command.kind = ChatCommandKind::kOwner;
    command.text = std::string(CommandArgs(text, "/owner"));
    return command;
  }

  if (MatchesCommand(text, "/rename")) {
    command.kind = ChatCommandKind::kRename;
    command.text = std::string(CommandArgs(text, "/rename"));
    if (command.text.empty()) {
      return reject(ChatCommandError::kBadRename);
    }
    return command;
  }

  if (MatchesCommand(text, "/portrait")) {
    command.kind = ChatCommandKind::kPortrait;
    command.text = std::string(CommandArgs(text, "/portrait"));
    if (command.text == "off") {
      command.text.clear();
      return command;
    }
    if (!IsSafeImageName(command.text)) {
      return reject(ChatCommandError::kBadPortrait);
    }
    return command;
  }

  if (MatchesCommand(text, "/maxhp")) {
    command.kind = ChatCommandKind::kMaxHp;
    if (!ParsePositiveNumber(CommandArgs(text, "/maxhp"), command.value)) {
      return reject(ChatCommandError::kBadMaxHp);
    }
    return command;
  }

  if (MatchesCommand(text, "/damage") || MatchesCommand(text, "/heal")) {
    const bool heal = MatchesCommand(text, "/heal");
    command.kind = heal ? ChatCommandKind::kHeal : ChatCommandKind::kDamage;
    if (!ParsePositiveNumber(CommandArgs(text, heal ? "/heal" : "/damage"),
                             command.value)) {
      return reject(ChatCommandError::kBadDamage);
    }
    return command;
  }

  if (text == "/deathsave") {
    command.kind = ChatCommandKind::kDeathSave;
    return command;
  }

  if (text == "/rest" || text == "/longrest") {
    command.kind = ChatCommandKind::kRest;
    command.long_rest = text == "/longrest";
    return command;
  }

  if (text == "/reveal") {
    command.kind = ChatCommandKind::kReveal;
    return command;
  }

  if (text == "/reveal off") {
    command.kind = ChatCommandKind::kRevealOff;
    return command;
  }

  if (MatchesCommand(text, "/reveal")) {
    return reject(ChatCommandError::kBadReveal);
  }

  if (MatchesCommand(text, "/xp")) {
    command.kind = ChatCommandKind::kXp;
    const std::string_view args = CommandArgs(text, "/xp");
    const std::size_t space = args.find(' ');
    int amount = 0;
    if (space == std::string_view::npos) {
      if (!ParsePositiveNumber(args, amount)) {
        return reject(ChatCommandError::kBadXp);
      }
    } else {
      if (args.substr(space + 1) != "all" ||
          !ParsePositiveNumber(args.substr(0, space), amount)) {
        return reject(ChatCommandError::kBadXp);
      }
      command.party = true;
    }
    command.value = amount;
    return command;
  }

  // Неизвестная команда — обычное сообщение: стол может обсуждать «/что-то».
  return command;
}

}  // namespace dnd
