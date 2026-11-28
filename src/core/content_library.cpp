#include "core/content_library.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <utility>

#include "core/monster_catalog.h"

namespace dnd {
namespace {

struct ContentFileSpec {
  std::string_view path;
  bool required;
};

auto ReadFile(const std::filesystem::path& path, std::string& text) -> bool {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  text.assign(std::istreambuf_iterator<char>(file),
              std::istreambuf_iterator<char>());
  return !file.bad();
}

auto MergeById(std::vector<MonsterTemplate>& target,
               std::vector<MonsterTemplate> source) -> void {
  for (MonsterTemplate& monster : source) {
    const auto existing = std::ranges::find_if(
        target, [&monster](const MonsterTemplate& other) -> bool {
          return other.id == monster.id;
        });
    if (existing != target.end()) {
      *existing = std::move(monster);
    } else {
      target.push_back(std::move(monster));
    }
  }
}

}  // namespace

auto LoadContentLibrary(const std::filesystem::path& base) -> ContentLibrary {
  ContentLibrary library;
  library.monsters = DefaultBestiary();
  constexpr std::array<ContentFileSpec, 2> files{{
      ContentFileSpec{.path = kShippedMonsterContent, .required = true},
      ContentFileSpec{.path = kLocalMonsterContent, .required = false},
  }};
  for (const ContentFileSpec& spec : files) {
    const std::filesystem::path path = base / std::string(spec.path);
    std::string text;
    if (!ReadFile(path, text)) {
      if (spec.required && library.warning.empty()) {
        library.warning =
            std::string(spec.path) + " не найден — встроенный бестиарий";
      }
      continue;
    }
    std::string error;
    auto monsters = ParseMonsterContent(text, error);
    if (!monsters.has_value()) {
      if (library.warning.empty()) {
        library.warning = std::string(spec.path) + ": " + error;
      }
      continue;
    }
    MergeById(library.monsters, std::move(*monsters));
  }
  return library;
}

}  // namespace dnd
