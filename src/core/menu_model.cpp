#include "core/menu_model.h"

#include <array>
#include <charconv>
#include <system_error>

namespace dnd {
namespace {

constexpr std::array<MenuField, 2> kHostFields{
    MenuField::kPort,
    MenuField::kPassword,
};
constexpr std::array<MenuField, 4> kJoinFields{
    MenuField::kAddress,
    MenuField::kPort,
    MenuField::kName,
    MenuField::kPassword,
};
constexpr int kMenuTabCount = 3;

auto ParsePositiveInt(std::string_view text, int& value) -> bool {
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

}  // namespace

auto MenuFieldOrder(MenuTab tab) -> std::span<const MenuField> {
  switch (tab) {
    case MenuTab::kHost:
      return kHostFields;
    case MenuTab::kJoin:
      return kJoinFields;
    case MenuTab::kEditor:
      break;
  }
  return {};
}

auto NextMenuField(MenuTab tab, MenuField current, int direction) -> MenuField {
  const std::span<const MenuField> fields = MenuFieldOrder(tab);
  if (fields.empty()) {
    return MenuField::kNone;
  }
  const auto count = static_cast<int>(fields.size());
  int index = -1;
  for (int i = 0; i < count; ++i) {
    if (fields[static_cast<std::size_t>(i)] == current) {
      index = i;
      break;
    }
  }
  int next = 0;
  if (index < 0) {
    next = direction >= 0 ? 0 : count - 1;
  } else {
    next = (index + direction + count) % count;
  }
  return fields[static_cast<std::size_t>(next)];
}

auto NextMenuTab(MenuTab tab, int direction) -> MenuTab {
  const int index = (static_cast<int>(tab) + direction) % kMenuTabCount;
  return static_cast<MenuTab>((index + kMenuTabCount) % kMenuTabCount);
}

auto MenuFieldLimit(MenuField field) -> std::size_t {
  switch (field) {
    case MenuField::kAddress:
      return 128;
    case MenuField::kPort:
      return 5;
    case MenuField::kName:
      return 64;
    case MenuField::kPassword:
      return 128;
    case MenuField::kNone:
      break;
  }
  return 0;
}

auto ActiveMenuText(MenuState& menu) -> std::string* {
  if (menu.tab == MenuTab::kEditor) {
    return nullptr;
  }
  switch (menu.active_field) {
    case MenuField::kAddress:
      return menu.tab == MenuTab::kJoin ? &menu.address : nullptr;
    case MenuField::kPort:
      return &menu.port;
    case MenuField::kName:
      return menu.tab == MenuTab::kJoin ? &menu.name : nullptr;
    case MenuField::kPassword:
      return &menu.password;
    case MenuField::kNone:
      break;
  }
  return nullptr;
}

auto ParsePort(std::string_view text, std::uint16_t& port) -> bool {
  int value = 0;
  if (!ParsePositiveInt(text, value) || value > 65535) {
    return false;
  }
  port = static_cast<std::uint16_t>(value);
  return true;
}

auto SplitHostPort(std::string_view text) -> HostAddress {
  HostAddress address{.host = std::string(text)};
  const std::size_t colon = text.rfind(':');
  if (colon == std::string_view::npos || colon + 1 >= text.size()) {
    return address;
  }
  std::uint16_t port = 0;
  if (!ParsePort(text.substr(colon + 1), port)) {
    return address;
  }
  address.host = std::string(text.substr(0, colon));
  address.port = port;
  address.has_port = true;
  return address;
}

auto SummarizeParty(const Overlay& overlay, bool exists, bool mismatch)
    -> PartySummary {
  PartySummary summary;
  if (!exists) {
    return summary;
  }
  if (mismatch) {
    summary.state = PartyState::kMismatch;
    return summary;
  }
  if (!HasOverlayChanges(overlay)) {
    summary.state = PartyState::kNew;
    return summary;
  }
  summary.state = PartyState::kContinued;
  summary.changed_scenes = overlay.scenes.size();
  for (const OverlayScene& scene : overlay.scenes) {
    if (scene.snapshot.has_value()) {
      ++summary.played_scenes;
    }
  }
  return summary;
}

auto MenuCommandForChar(MenuTab tab, char key) -> MenuCommand {
  if (tab != MenuTab::kHost) {
    return MenuCommand::kNone;
  }
  switch (key) {
    case 'n':
    case 'N':
      return MenuCommand::kNewParty;
    case 'r':
    case 'R':
      return MenuCommand::kResetScene;
    case 'a':
    case 'A':
      return MenuCommand::kAcceptParty;
    default:
      return MenuCommand::kNone;
  }
}

}  // namespace dnd
