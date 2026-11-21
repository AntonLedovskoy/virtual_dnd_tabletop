#include "core/version.h"

#include <charconv>
#include <string_view>
#include <system_error>

namespace dnd {

namespace {

auto ParseNumber(std::string_view text, int& value) -> bool {
  if (text.empty()) {
    return false;
  }
  int parsed = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      parsed < 0) {
    return false;
  }
  value = parsed;
  return true;
}

auto CompareInt(int left, int right) -> int {
  if (left == right) {
    return 0;
  }
  return left < right ? -1 : 1;
}

}  // namespace

auto ParseVersion(std::string_view text, Version& version) -> bool {
  if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) {
    text.remove_prefix(1);
  }
  const auto dash = text.find('-');
  if (dash != std::string_view::npos && dash + 1 == text.size()) {
    return false;  // Пустой суффикс: `0.5.2-`.
  }
  const std::string_view numbers = text.substr(0, dash);
  const auto first_dot = numbers.find('.');
  if (first_dot == std::string_view::npos) {
    return false;
  }
  const auto second_dot = numbers.find('.', first_dot + 1);
  if (second_dot == std::string_view::npos ||
      numbers.find('.', second_dot + 1) != std::string_view::npos) {
    return false;
  }

  Version parsed;
  if (!ParseNumber(numbers.substr(0, first_dot), parsed.major) ||
      !ParseNumber(numbers.substr(first_dot + 1, second_dot - first_dot - 1),
                   parsed.minor) ||
      !ParseNumber(numbers.substr(second_dot + 1), parsed.patch)) {
    return false;
  }
  parsed.dirty = dash != std::string_view::npos;
  version = parsed;
  return true;
}

auto CompareVersion(const Version& left, const Version& right) -> int {
  if (const int order = CompareInt(left.major, right.major); order != 0) {
    return order;
  }
  if (const int order = CompareInt(left.minor, right.minor); order != 0) {
    return order;
  }
  return CompareInt(left.patch, right.patch);
}

auto FormatVersion(const Version& version) -> std::string {
  std::string text = std::to_string(version.major);
  text += '.';
  text += std::to_string(version.minor);
  text += '.';
  text += std::to_string(version.patch);
  if (version.dirty) {
    text += "-dirty";
  }
  return text;
}

}  // namespace dnd
