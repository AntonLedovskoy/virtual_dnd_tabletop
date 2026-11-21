#include "core/log_history.h"

#include <algorithm>
#include <fstream>
#include <string>

#include "core/player_name.h"

namespace dnd {
namespace {

auto TrimCarriageReturn(std::string_view line) -> std::string_view {
  if (!line.empty() && line.back() == '\r') {
    line.remove_suffix(1);
  }
  return line;
}

// Разбирает одну строку формата «[время] автор: текст». Строку без времени
// целиком считаем текстом: отделить автора от сообщения надёжно нельзя.
auto ParseHistoryLine(std::string_view raw) -> HistoryLine {
  HistoryLine line;
  constexpr std::string_view header_open = "=== ";
  constexpr std::string_view header_close = " ===";
  if (raw.size() >= header_open.size() + header_close.size() &&
      raw.starts_with(header_open) &&
      raw.substr(raw.size() - header_close.size()) == header_close) {
    line.header = true;
    std::string inner(
        raw.substr(header_open.size(),
                   raw.size() - header_open.size() - header_close.size()));
    constexpr std::string_view session_prefix = "сессия ";
    if (inner.starts_with(session_prefix)) {
      inner.erase(0, session_prefix.size());
    }
    line.time = inner;
    return line;
  }

  std::string_view rest = raw;
  if (raw.front() == '[') {
    const auto close = raw.find("] ");
    if (close != std::string_view::npos && close > 1) {
      line.time = std::string(raw.substr(1, close - 1));
      rest = raw.substr(close + 2);
    }
  }
  if (!line.time.empty()) {
    const auto colon = rest.find(": ");
    if (colon != std::string_view::npos) {
      line.author = std::string(rest.substr(0, colon));
      line.text = std::string(rest.substr(colon + 2));
      return line;
    }
  }
  line.text = std::string(rest);
  return line;
}

auto MatchesQuery(const HistoryLine& line, const std::string& key) -> bool {
  const std::string haystack = PlayerNameKey(
      line.author.empty() ? line.text : line.author + " " + line.text);
  return haystack.find(key) != std::string::npos;
}

}  // namespace

auto ParseSessionLog(std::string_view text, std::size_t max_lines)
    -> std::vector<HistoryLine> {
  std::vector<HistoryLine> lines;
  std::size_t index = 0;
  while (index < text.size()) {
    const auto end = text.find('\n', index);
    const std::size_t length =
        end == std::string_view::npos ? text.size() - index : end - index;
    const std::string_view raw = TrimCarriageReturn(text.substr(index, length));
    index = end == std::string_view::npos ? text.size() : end + 1;
    if (raw.empty()) {
      continue;
    }
    lines.push_back(ParseHistoryLine(raw));
  }
  if (lines.size() > max_lines) {
    lines.erase(lines.begin(),
                lines.end() - static_cast<std::ptrdiff_t>(max_lines));
  }
  return lines;
}

auto LoadSessionLog(const std::string& path, std::size_t max_lines)
    -> std::vector<HistoryLine> {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  if (size <= 0) {
    return {};
  }
  const auto tail = static_cast<std::streamoff>(kMaxHistoryBytes);
  const std::streamoff start = std::max<std::streamoff>(0, size - tail);
  in.seekg(start);
  std::string content;
  content.resize(static_cast<std::size_t>(size - start));
  in.read(content.data(), static_cast<std::streamsize>(content.size()));
  content.resize(static_cast<std::size_t>(in.gcount()));

  // Чтение с середины строки: первую неполную строку выбрасываем, иначе в
  // журнале появится обрезанное сообщение.
  if (start > 0) {
    const auto newline = content.find('\n');
    if (newline == std::string::npos) {
      return {};
    }
    content.erase(0, newline + 1);
  }
  return ParseSessionLog(content, max_lines);
}

auto SearchSessionLog(const std::vector<HistoryLine>& lines,
                      std::string_view query) -> std::vector<std::size_t> {
  std::vector<std::size_t> matches;
  if (query.empty()) {
    matches.reserve(lines.size());
    for (std::size_t i = 0; i < lines.size(); ++i) {
      matches.push_back(i);
    }
    return matches;
  }

  const std::string key = PlayerNameKey(query);
  std::size_t i = 0;
  while (i < lines.size()) {
    const bool has_header = lines[i].header;
    const std::size_t header = i;
    if (has_header) {
      ++i;
    }
    std::vector<std::size_t> body;
    while (i < lines.size() && !lines[i].header) {
      if (MatchesQuery(lines[i], key)) {
        body.push_back(i);
      }
      ++i;
    }
    if (body.empty()) {
      continue;
    }
    if (has_header) {
      matches.push_back(header);
    }
    matches.insert(matches.end(), body.begin(), body.end());
  }
  return matches;
}

}  // namespace dnd
