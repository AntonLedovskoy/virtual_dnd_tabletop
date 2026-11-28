#include "core/content_parser.h"

#include <algorithm>
#include <cstddef>

namespace dnd {
namespace {

auto Trim(std::string_view text) -> std::string_view {
  const auto first = text.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t");
  return text.substr(first, last - first + 1);
}

auto Fail(std::string& error, int line, std::string_view message) -> bool {
  error = "строка " + std::to_string(line) + ": " + std::string(message);
  return false;
}

auto FindSchema(std::span<const ContentSchema> schemas, std::string_view type)
    -> const ContentSchema* {
  for (const ContentSchema& schema : schemas) {
    if (schema.type == type) {
      return &schema;
    }
  }
  return nullptr;
}

auto HasKey(const ContentSchema& schema, std::string_view key) -> bool {
  return std::ranges::any_of(
      schema.keys,
      [key](std::string_view candidate) -> bool { return candidate == key; });
}

auto HasEntry(const ContentSection& section, std::string_view key) -> bool {
  return section.Find(key) != nullptr;
}

}  // namespace

auto ContentSection::Find(std::string_view key) const -> const ContentEntry* {
  for (const ContentEntry& entry : entries) {
    if (entry.key == key) {
      return &entry;
    }
  }
  return nullptr;
}

auto ParseContent(std::string_view text, std::span<const ContentSchema> schemas,
                  ContentFile& file, std::string& error) -> bool {
  file = ContentFile{};
  error.clear();
  // BOM от «Блокнота» не должен попадать в первую строку.
  constexpr std::string_view bom = "\xEF\xBB\xBF";
  if (text.starts_with(bom)) {
    text.remove_prefix(bom.size());
  }

  const ContentSchema* schema = nullptr;
  ContentSection* section = nullptr;
  bool saw_format = false;
  int line_number = 0;
  std::size_t position = 0;
  while (position <= text.size()) {
    std::size_t end = text.find('\n', position);
    if (end == std::string_view::npos) {
      end = text.size();
    }
    std::string_view line = text.substr(position, end - position);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    line = Trim(line);
    ++line_number;
    const bool last_line = end == text.size();
    position = last_line ? text.size() + 1 : end + 1;
    if (line.empty() || line.front() == '#') {
      continue;
    }

    if (line.front() == '[') {
      if (line.size() < 2 || line.back() != ']') {
        return Fail(error, line_number, "секция должна быть [тип id]");
      }
      const std::string_view inner = Trim(line.substr(1, line.size() - 2));
      const std::size_t split = inner.find_first_of(" \t");
      if (split == std::string_view::npos) {
        return Fail(error, line_number, "секция должна быть [тип id]");
      }
      const std::string_view type = inner.substr(0, split);
      const std::string_view id = Trim(inner.substr(split));
      if (id.empty() || id.find_first_of(" \t") != std::string_view::npos) {
        return Fail(error, line_number, "секция должна быть [тип id]");
      }
      const ContentSchema* found = FindSchema(schemas, type);
      if (found == nullptr) {
        return Fail(error, line_number,
                    "незнакомый тип секции «" + std::string(type) + "»");
      }
      file.sections.push_back(ContentSection{.type = std::string(type),
                                             .id = std::string(id),
                                             .line = line_number});
      section = &file.sections.back();
      schema = found;
      continue;
    }

    const std::size_t equal = line.find('=');
    if (equal == std::string_view::npos) {
      return Fail(error, line_number, "ожидается [тип id] или ключ = значение");
    }
    const std::string_view key = Trim(line.substr(0, equal));
    const std::string_view value = Trim(line.substr(equal + 1));
    if (key.empty()) {
      return Fail(error, line_number, "пустой ключ");
    }
    // `format` допустим только до первой секции, иначе это незнакомый ключ.
    if (section == nullptr) {
      if (key != "format") {
        return Fail(error, line_number, "ключ вне секции");
      }
      if (saw_format) {
        return Fail(error, line_number, "повторная строка format = 1");
      }
      if (value != "1") {
        return Fail(error, line_number, "поддерживается только format = 1");
      }
      file.format = 1;
      saw_format = true;
      continue;
    }
    if (schema == nullptr) {
      return Fail(error, line_number, "ключ вне секции");
    }
    if (!HasKey(*schema, key)) {
      return Fail(error, line_number,
                  "незнакомый ключ «" + std::string(key) + "»");
    }
    if (HasEntry(*section, key)) {
      return Fail(error, line_number,
                  "повтор ключа «" + std::string(key) + "»");
    }
    section->entries.push_back(ContentEntry{.key = std::string(key),
                                            .value = std::string(value),
                                            .line = line_number});
  }

  if (!saw_format) {
    error = "нет строки format = 1";
    return false;
  }
  return true;
}

}  // namespace dnd
