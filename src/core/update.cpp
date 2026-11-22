#include "core/update.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "core/sha256.h"

namespace dnd {
namespace {

// Глубже не разбираем: настоящий манифест плоский, а защита от рекурсии
// нужна, чтобы битый файл не уронил разбор.
constexpr int kMaxDepth = 16;

auto IsSpace(char c) -> bool {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

auto IsHexDigit(char c) -> bool {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

auto HexValue(char c) -> int {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  return c - 'A' + 10;
}

auto AppendCodePoint(std::string& out, std::uint32_t code_point) -> void {
  if (code_point <= 0x7F) {
    out.push_back(static_cast<char>(code_point));
  } else if (code_point <= 0x7FF) {
    out.push_back(static_cast<char>(0xC0 | (code_point >> 6)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else if (code_point <= 0xFFFF) {
    out.push_back(static_cast<char>(0xE0 | (code_point >> 12)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (code_point >> 18)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((code_point >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (code_point & 0x3F)));
  }
}

// Разборщик одного JSON-значения: указатель, позиция и запас по глубине.
class Reader {
 public:
  Reader(std::string_view text, std::size_t begin) : text_(text), pos_(begin) {}

  [[nodiscard]] auto Position() const -> std::size_t { return pos_; }

  auto SkipWhitespace() -> void {
    while (pos_ < text_.size() && IsSpace(text_[pos_])) {
      ++pos_;
    }
  }

  // Строка в кавычках с JSON-экранированием, включая \uXXXX.
  [[nodiscard]] auto ParseString(std::string& out) -> bool {
    SkipWhitespace();
    if (pos_ >= text_.size() || text_[pos_] != '"') {
      return false;
    }
    ++pos_;
    out.clear();
    while (pos_ < text_.size()) {
      const char c = text_[pos_++];
      if (c == '"') {
        return true;
      }
      if (c != '\\') {
        if (static_cast<unsigned char>(c) < 0x20) {
          return false;
        }
        out.push_back(c);
        continue;
      }
      if (pos_ >= text_.size()) {
        return false;
      }
      const char escape = text_[pos_++];
      switch (escape) {
        case '"':
          out.push_back('"');
          break;
        case '\\':
          out.push_back('\\');
          break;
        case '/':
          out.push_back('/');
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          std::uint32_t code_point = 0;
          if (!ParseHex4(code_point)) {
            return false;
          }
          // Суррогатная пара: высокий суррогат без пары заменяем на U+FFFD,
          // низкий одиночный — тоже: текст заметок не важнее устойчивости.
          if (code_point >= 0xD800 && code_point <= 0xDBFF) {
            const std::size_t after_high = pos_;
            if (pos_ + 1 < text_.size() && text_[pos_] == '\\' &&
                text_[pos_ + 1] == 'u') {
              pos_ += 2;
              std::uint32_t low = 0;
              if (!ParseHex4(low)) {
                return false;
              }
              if (low >= 0xDC00 && low <= 0xDFFF) {
                code_point =
                    0x10000 + ((code_point - 0xD800) << 10) + (low - 0xDC00);
              } else {
                code_point = 0xFFFD;
              }
            } else {
              pos_ = after_high;
              code_point = 0xFFFD;
            }
          } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
            code_point = 0xFFFD;
          }
          AppendCodePoint(out, code_point);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }

  // Пропускает любое значение, проверяя только структуру. depth ограничивает
  // вложенность; unknown-поля манифеста не должны ломать разбор.
  // NOLINTNEXTLINE(misc-no-recursion): рекурсия ограничена kMaxDepth.
  [[nodiscard]] auto SkipValue(int depth) -> bool {
    if (depth > kMaxDepth) {
      return false;
    }
    SkipWhitespace();
    if (pos_ >= text_.size()) {
      return false;
    }
    const char c = text_[pos_];
    if (c == '"') {
      std::string ignored;
      return ParseString(ignored);
    }
    if (c == '{') {
      ++pos_;
      SkipWhitespace();
      if (pos_ < text_.size() && text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      while (pos_ < text_.size()) {
        std::string key;
        if (!ParseString(key)) {
          return false;
        }
        SkipWhitespace();
        if (pos_ >= text_.size() || text_[pos_] != ':') {
          return false;
        }
        ++pos_;
        if (!SkipValue(depth + 1)) {
          return false;
        }
        SkipWhitespace();
        if (pos_ >= text_.size()) {
          return false;
        }
        if (text_[pos_] == '}') {
          ++pos_;
          return true;
        }
        if (text_[pos_] != ',') {
          return false;
        }
        ++pos_;
      }
      return false;
    }
    if (c == '[') {
      ++pos_;
      SkipWhitespace();
      if (pos_ < text_.size() && text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      while (pos_ < text_.size()) {
        if (!SkipValue(depth + 1)) {
          return false;
        }
        SkipWhitespace();
        if (pos_ >= text_.size()) {
          return false;
        }
        if (text_[pos_] == ']') {
          ++pos_;
          return true;
        }
        if (text_[pos_] != ',') {
          return false;
        }
        ++pos_;
      }
      return false;
    }
    if (c == 't') {
      return ConsumeLiteral("true");
    }
    if (c == 'f') {
      return ConsumeLiteral("false");
    }
    if (c == 'n') {
      return ConsumeLiteral("null");
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      SkipNumber();
      return true;
    }
    return false;
  }

  // Неотрицательное целое без знака и точек: protocol — маленькое число.
  [[nodiscard]] auto ParseInteger(int& out) -> bool {
    SkipWhitespace();
    const std::size_t start = pos_;
    std::int64_t value = 0;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      value = value * 10 + (text_[pos_] - '0');
      if (value > 1'000'000'000) {
        return false;
      }
      ++pos_;
    }
    if (pos_ == start) {
      return false;
    }
    out = static_cast<int>(value);
    return true;
  }

  [[nodiscard]] auto Consume(char expected) -> bool {
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == expected) {
      ++pos_;
      return true;
    }
    return false;
  }

  // Неотрицательное целое с потолком: размер файла из манифеста.
  [[nodiscard]] auto ParseUnsigned(std::uint64_t limit, std::uint64_t& out)
      -> bool {
    SkipWhitespace();
    const std::size_t start = pos_;
    std::uint64_t value = 0;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      value = value * 10 + static_cast<std::uint64_t>(text_[pos_] - '0');
      if (value > limit) {
        return false;
      }
      ++pos_;
    }
    if (pos_ == start) {
      return false;
    }
    out = value;
    return true;
  }

 private:
  [[nodiscard]] auto ParseHex4(std::uint32_t& value) -> bool {
    if (pos_ + 4 > text_.size()) {
      return false;
    }
    value = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_++];
      if (!IsHexDigit(c)) {
        return false;
      }
      value = (value << 4) | static_cast<std::uint32_t>(HexValue(c));
    }
    return true;
  }

  [[nodiscard]] auto ConsumeLiteral(std::string_view literal) -> bool {
    if (text_.substr(pos_, literal.size()) != literal) {
      return false;
    }
    pos_ += literal.size();
    return true;
  }

  auto SkipNumber() -> void {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if ((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.' ||
          c == 'e' || c == 'E') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  std::string_view text_;
  std::size_t pos_ = 0;
};

[[nodiscard]] auto IsValidSha256(std::string_view value) -> bool {
  if (value.size() != 64) {
    return false;
  }
  return std::ranges::all_of(value, IsHexDigit);
}

auto ToLowerAscii(std::string& text) -> void {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
}

auto ToLowerAsciiCopy(std::string_view text) -> std::string {
  std::string copy(text);
  ToLowerAscii(copy);
  return copy;
}

auto EndsWithNoCase(std::string_view text, std::string_view suffix) -> bool {
  if (text.size() < suffix.size()) {
    return false;
  }
  return ToLowerAsciiCopy(text.substr(text.size() - suffix.size())) ==
         ToLowerAsciiCopy(suffix);
}

// Один элемент массива `files`. Неизвестные ключи пропускаем: манифест
// будущей версии не должен ломать старый клиент. Обязательны все четыре
// поля: без пути, ссылки, хеша или размера файл не скачать и не проверить.
[[nodiscard]] auto ParseUpdateFile(Reader& reader,
                                   const std::vector<UpdateFile>& already,
                                   UpdateFile& file) -> bool {
  if (!reader.Consume('{')) {
    return false;
  }
  reader.SkipWhitespace();
  if (reader.Consume('}')) {
    return false;
  }
  bool has_path = false;
  bool has_url = false;
  bool has_sha = false;
  bool has_size = false;
  while (true) {
    std::string key;
    if (!reader.ParseString(key)) {
      return false;
    }
    if (!reader.Consume(':')) {
      return false;
    }
    if (key == "path") {
      std::string text;
      if (!reader.ParseString(text) || !IsSafeUpdatePath(text)) {
        return false;
      }
      // Windows не различает регистр: два пути, отличающиеся им, — один файл.
      const std::string folded = ToLowerAsciiCopy(text);
      for (const UpdateFile& other : already) {
        if (ToLowerAsciiCopy(other.path) == folded) {
          return false;
        }
      }
      file.path = std::move(text);
      has_path = true;
    } else if (key == "url") {
      std::string text;
      if (!reader.ParseString(text) || text.empty()) {
        return false;
      }
      file.url = std::move(text);
      has_url = true;
    } else if (key == "sha256") {
      std::string text;
      if (!reader.ParseString(text) || !IsValidSha256(text)) {
        return false;
      }
      ToLowerAscii(text);
      file.sha256 = std::move(text);
      has_sha = true;
    } else if (key == "size") {
      std::uint64_t size = 0;
      if (!reader.ParseUnsigned(kMaxUpdateBytes, size) || size == 0) {
        return false;
      }
      file.size = size;
      has_size = true;
    } else if (!reader.SkipValue(1)) {
      return false;
    }

    reader.SkipWhitespace();
    if (reader.Consume('}')) {
      break;
    }
    if (!reader.Consume(',')) {
      return false;
    }
  }
  return has_path && has_url && has_sha && has_size;
}

[[nodiscard]] auto ParseUpdateFiles(Reader& reader,
                                    std::vector<UpdateFile>& out) -> bool {
  out.clear();
  if (!reader.Consume('[')) {
    return false;
  }
  reader.SkipWhitespace();
  if (reader.Consume(']')) {
    return true;
  }
  std::uint64_t total = 0;
  while (true) {
    if (out.size() >= kMaxUpdateFiles) {
      return false;
    }
    UpdateFile file;
    if (!ParseUpdateFile(reader, out, file)) {
      return false;
    }
    total += file.size;
    if (total > kMaxUpdateTotalBytes) {
      return false;
    }
    out.push_back(std::move(file));
    reader.SkipWhitespace();
    if (reader.Consume(']')) {
      return true;
    }
    if (!reader.Consume(',')) {
      return false;
    }
  }
}

// sha256 файла на диске: файл читается целиком, как и при скачивании.
// false — файла нет или он больше лимита: тогда его нужно качать.
[[nodiscard]] auto HashFileHex(const std::filesystem::path& path,
                               std::string& hex) -> bool {
  std::error_code code;
  const std::uintmax_t size = std::filesystem::file_size(path, code);
  if (code || size == 0 || size > kMaxUpdateBytes) {
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return false;
  }
  std::string bytes(static_cast<std::size_t>(size), '\0');
  file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!file) {
    return false;
  }
  hex = HexDigest(Sha256(std::string_view(bytes)));
  return true;
}

}  // namespace

auto IsSafeUpdatePath(std::string_view path) -> bool {
  constexpr std::size_t max_path_bytes = 200;
  constexpr std::size_t max_segment_bytes = 64;
  if (path.empty() || path.size() > max_path_bytes) {
    return false;
  }
  // Только `assets/`: данные стола и сам exe манифесту недоступны.
  if (!path.starts_with(kAssetDirectory)) {
    return false;
  }
  // Временное имя скачанного файла — наше: ассет с таким именем перетрёт его.
  if (EndsWithNoCase(path, kStagedSuffix)) {
    return false;
  }
  std::size_t begin = 0;
  while (begin <= path.size()) {
    const std::size_t slash = path.find('/', begin);
    const std::string_view segment = slash == std::string_view::npos
                                         ? path.substr(begin)
                                         : path.substr(begin, slash - begin);
    if (segment.empty() || segment.size() > max_segment_bytes ||
        segment == "." || segment == ".." || segment.back() == '.') {
      return false;
    }
    for (const char c : segment) {
      const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '.' || c == '-' ||
                           c == '_';
      if (!allowed) {
        return false;
      }
    }
    if (slash == std::string_view::npos) {
      return true;
    }
    begin = slash + 1;
  }
  return false;
}

auto StagedUpdatePath(const std::filesystem::path& base, std::string_view path)
    -> std::filesystem::path {
  std::string staged(path);
  staged += kStagedSuffix;
  return base / std::filesystem::path(staged);
}

auto PlanAssetDownloads(const std::filesystem::path& base,
                        const std::vector<UpdateFile>& files,
                        UpdateDownloadPlan& plan) -> void {
  UpdateDownloadPlan result;
  for (const UpdateFile& file : files) {
    const std::filesystem::path target =
        base / std::filesystem::path(file.path);
    std::string hex;
    if (!HashFileHex(target, hex) || hex != file.sha256) {
      result.files.push_back(file);
      result.total_bytes += file.size;
    }
  }
  plan = std::move(result);
}

auto RemoveStagedAssets(const std::filesystem::path& base) -> void {
  const std::filesystem::path root =
      base / std::filesystem::path(std::string(kAssetDirectory));
  std::error_code code;
  if (!std::filesystem::is_directory(root, code)) {
    return;
  }
  // Сначала собираем, потом удаляем: менять дерево во время обхода нельзя.
  std::vector<std::filesystem::path> staged;
  std::filesystem::recursive_directory_iterator it(root, code);
  const std::filesystem::recursive_directory_iterator end;
  if (code) {
    return;
  }
  for (; it != end; it.increment(code)) {
    if (code) {
      break;
    }
    if (!it->is_regular_file(code)) {
      continue;
    }
    if (EndsWithNoCase(it->path().filename().string(), kStagedSuffix)) {
      staged.push_back(it->path());
    }
  }
  for (const std::filesystem::path& path : staged) {
    std::filesystem::remove(path, code);
    code.clear();
  }
}

auto ParseUpdateManifest(std::string_view json, UpdateManifest& manifest)
    -> bool {
  Reader reader(json, 0);
  reader.SkipWhitespace();
  if (!reader.Consume('{')) {
    return false;
  }

  UpdateManifest parsed;
  bool has_version = false;
  bool has_url = false;
  bool has_sha = false;

  reader.SkipWhitespace();
  if (reader.Consume('}')) {
    return false;
  }
  while (true) {
    std::string key;
    if (!reader.ParseString(key)) {
      return false;
    }
    if (!reader.Consume(':')) {
      return false;
    }
    reader.SkipWhitespace();

    if (key == "version") {
      std::string text;
      if (!reader.ParseString(text)) {
        return false;
      }
      Version version;
      if (!ParseVersion(text, version)) {
        return false;
      }
      parsed.version = version;
      has_version = true;
    } else if (key == "url") {
      std::string text;
      if (!reader.ParseString(text)) {
        return false;
      }
      parsed.url = std::move(text);
      has_url = !parsed.url.empty();
    } else if (key == "sha256") {
      std::string text;
      if (!reader.ParseString(text)) {
        return false;
      }
      if (!IsValidSha256(text)) {
        return false;
      }
      ToLowerAscii(text);
      parsed.sha256 = std::move(text);
      has_sha = true;
    } else if (key == "notes") {
      std::string text;
      if (!reader.ParseString(text)) {
        return false;
      }
      parsed.notes = std::move(text);
    } else if (key == "published") {
      std::string text;
      if (!reader.ParseString(text)) {
        return false;
      }
      parsed.published = std::move(text);
    } else if (key == "protocol") {
      int protocol = 0;
      if (!reader.ParseInteger(protocol)) {
        return false;
      }
      parsed.protocol = protocol;
    } else if (key == "files") {
      // Битая таблица ассетов — битый манифест: без части файлов установка
      // окажется половинчатой, лучше отказаться целиком.
      if (!ParseUpdateFiles(reader, parsed.files)) {
        return false;
      }
    } else if (!reader.SkipValue(0)) {
      return false;
    }

    reader.SkipWhitespace();
    if (reader.Consume('}')) {
      break;
    }
    if (!reader.Consume(',')) {
      return false;
    }
  }

  // Хвост после объекта: только пробелы. Мусор после `}` — битый файл.
  reader.SkipWhitespace();
  if (reader.Position() != json.size()) {
    return false;
  }
  if (!has_version || !has_url || !has_sha) {
    return false;
  }

  manifest = std::move(parsed);
  return true;
}

auto IsUpdateAvailable(const Version& current, const UpdateManifest& manifest,
                       bool allow_developer_build) -> bool {
  if (current.dirty && !allow_developer_build) {
    return false;
  }
  return CompareVersion(manifest.version, current) > 0;
}

auto AdviseUpdate(const Version& mine, const Version& host,
                  const std::optional<Version>& latest) -> UpdateAdvice {
  // Сначала стороны: кто отстал, решает только сравнение с хостом, а
  // наличие более новой версии важно лишь когда версии совпали.
  const int against_host = CompareVersion(mine, host);
  if (against_host < 0) {
    return UpdateAdvice::kUpdateMine;
  }
  if (against_host > 0) {
    return UpdateAdvice::kUpdateHost;
  }
  if (!latest.has_value()) {
    return UpdateAdvice::kUnknown;
  }
  return CompareVersion(*latest, mine) > 0 ? UpdateAdvice::kNewerAvailable
                                           : UpdateAdvice::kUpToDate;
}

auto PlanStartupRecovery(bool have_exe, bool have_old, bool have_update)
    -> StartupAction {
  if (have_exe) {
    // Любой остаток прошлой подмены убирается: рабочий exe уже на месте.
    if (have_old || have_update) {
      return StartupAction::kCleanup;
    }
    return StartupAction::kNone;
  }
  // exe нет, но есть старый: он заведомо целый, возвращаем его.
  if (have_old) {
    return StartupAction::kRestoreOld;
  }
  // exe нет, есть только скачанный: подмена дошла до первого шага.
  if (have_update) {
    return StartupAction::kFinishSwap;
  }
  return StartupAction::kNone;
}

auto ApplyStartupRecovery(const std::filesystem::path& directory,
                          StartupAction action, std::string& error) -> bool {
  const std::filesystem::path exe = directory / std::string(kExecutableName);
  const std::filesystem::path old_exe =
      directory / std::string(kPreviousExeName);
  const std::filesystem::path update = directory / std::string(kUpdateExeName);
  std::error_code code;

  // Недокачанные ассеты убираются при любом действии: рабочий exe их не
  // читает, а места они не занимают.
  RemoveStagedAssets(directory);

  switch (action) {
    case StartupAction::kNone:
      return true;
    case StartupAction::kRestoreOld:
      if (!std::filesystem::exists(old_exe, code)) {
        error = "нет " + old_exe.string() + " для восстановления";
        return false;
      }
      std::filesystem::rename(old_exe, exe, code);
      if (code) {
        error =
            "не удалось вернуть " + old_exe.string() + ": " + code.message();
        return false;
      }
      return true;
    case StartupAction::kFinishSwap:
      if (!std::filesystem::exists(update, code)) {
        error = "нет " + update.string() + " для завершения подмены";
        return false;
      }
      std::filesystem::rename(update, exe, code);
      if (code) {
        error = "не удалось завершить подмену: " + code.message();
        return false;
      }
      return true;
    case StartupAction::kCleanup:
      // Неудачное удаление (антивирус держит файл) не мешает запуску: файл
      // уберётся при следующем старте.
      std::filesystem::remove(old_exe, code);
      code.clear();
      std::filesystem::remove(update, code);
      return true;
  }
  return true;
}

}  // namespace dnd
