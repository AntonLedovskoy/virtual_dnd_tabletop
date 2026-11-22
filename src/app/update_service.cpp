#include "app/update_service.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <ios>
#include <system_error>
#include <utility>

#include "app/instance_lock.h"
#include "core/sha256.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace dnd {
namespace {

auto StartsWithNoCase(std::string_view text, std::string_view prefix) -> bool {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    const auto left = static_cast<unsigned char>(text[i]);
    const auto right = static_cast<unsigned char>(prefix[i]);
    if (std::tolower(left) != std::tolower(right)) {
      return false;
    }
  }
  return true;
}

auto IsRemoteUrl(std::string_view url) -> bool {
  return StartsWithNoCase(url, "http://") || StartsWithNoCase(url, "https://");
}

auto PathFromUtf8(std::string_view text) -> std::filesystem::path {
  const std::u8string utf8(reinterpret_cast<const char8_t*>(text.data()),
                           text.size());
  return {utf8};
}

auto ReadFileBytes(const std::filesystem::path& path, std::size_t max_bytes,
                   std::string& out, std::string& error) -> bool {
  std::error_code code;
  const std::uintmax_t size = std::filesystem::file_size(path, code);
  if (code) {
    error = "не удалось прочитать " + path.string() + ": " + code.message();
    return false;
  }
  if (size == 0) {
    error = "файл " + path.string() + " пуст";
    return false;
  }
  if (size > max_bytes) {
    error = "файл " + path.string() + " больше лимита";
    return false;
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    error = "не удалось открыть " + path.string();
    return false;
  }
  out.resize(static_cast<std::size_t>(size));
  file.read(out.data(), static_cast<std::streamsize>(out.size()));
  if (!file) {
    error = "не удалось дочитать " + path.string();
    return false;
  }
  return true;
}

auto WriteFileBytes(const std::filesystem::path& path, std::string_view bytes,
                    std::string& error) -> bool {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    error = "нет прав на запись в " + path.parent_path().string();
    return false;
  }
  file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  file.flush();
  if (!file) {
    error = "не удалось записать " + path.string();
    return false;
  }
  return true;
}

// Проба записи в папку игры: из Program Files подменить exe нельзя, и кнопка
// обновления должна вести в браузер, а не молча падать.
auto CanWriteDirectory(const std::filesystem::path& directory) -> bool {
  const std::filesystem::path probe =
      directory / ".virtual_dnd_tabletop.write_probe";
  {
    std::ofstream file(probe, std::ios::binary | std::ios::trunc);
    if (!file) {
      return false;
    }
    file.put('x');
    file.flush();
    if (!file) {
      std::error_code code;
      std::filesystem::remove(probe, code);
      return false;
    }
  }
  std::error_code code;
  std::filesystem::remove(probe, code);
  return true;
}

#ifdef _WIN32

auto Utf8ToWide(std::string_view text) -> std::wstring {
  if (text.empty()) {
    return {};
  }
  const int length = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring wide(static_cast<std::size_t>(length), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      wide.data(), length);
  return wide;
}

auto WideToUtf8(std::wstring_view text) -> std::string {
  if (text.empty()) {
    return {};
  }
  const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                         static_cast<int>(text.size()), nullptr,
                                         0, nullptr, nullptr);
  if (length <= 0) {
    return {};
  }
  std::string utf8(static_cast<std::size_t>(length), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      utf8.data(), length, nullptr, nullptr);
  return utf8;
}

auto ErrorText(DWORD code) -> std::string {
  wchar_t* buffer = nullptr;
  const DWORD length = FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
          FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  if (length == 0 || buffer == nullptr) {
    return "код " + std::to_string(code);
  }
  std::string text = WideToUtf8(std::wstring_view(buffer, length));
  LocalFree(buffer);
  while (!text.empty() &&
         (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) {
    text.pop_back();
  }
  return text.empty() ? "код " + std::to_string(code) : text;
}

// Закрывает дескрипторы WinHTTP в обратном порядке, в том числе на ошибке.
class HttpHandles {
 public:
  HttpHandles() = default;
  ~HttpHandles() {
    if (request_ != nullptr) {
      WinHttpCloseHandle(request_);
    }
    if (connect_ != nullptr) {
      WinHttpCloseHandle(connect_);
    }
    if (session_ != nullptr) {
      WinHttpCloseHandle(session_);
    }
  }
  HttpHandles(const HttpHandles&) = delete;
  auto operator=(const HttpHandles&) -> HttpHandles& = delete;

  [[nodiscard]] auto Session() -> HINTERNET& { return session_; }
  [[nodiscard]] auto Connect() -> HINTERNET& { return connect_; }
  [[nodiscard]] auto Request() -> HINTERNET& { return request_; }

 private:
  HINTERNET session_ = nullptr;
  HINTERNET connect_ = nullptr;
  HINTERNET request_ = nullptr;
};

struct HttpResult {
  bool ok = false;
  int status = 0;
  std::string body;
  std::string error;
};

// GET с редиректами: GitHub отдаёт 302 на объект релиза, WinHTTP идёт по нему
// сам. Тело целиком в памяти: манифест маленький, exe — десятки мегабайт.
auto HttpGet(std::string_view url, std::size_t max_bytes,
             const std::function<void(std::size_t, std::size_t)>& progress)
    -> HttpResult {
  HttpResult result;
  const std::wstring wide_url = Utf8ToWide(url);
  if (wide_url.empty()) {
    result.error = "пустой адрес";
    return result;
  }

  URL_COMPONENTS parts{};
  parts.dwStructSize = sizeof(parts);
  parts.dwSchemeLength = static_cast<DWORD>(-1);
  parts.dwHostNameLength = static_cast<DWORD>(-1);
  parts.dwUrlPathLength = static_cast<DWORD>(-1);
  parts.dwExtraInfoLength = static_cast<DWORD>(-1);
  if (WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0,
                      &parts) == FALSE) {
    result.error = "некорректный адрес: " + std::string(url);
    return result;
  }
  if (parts.nScheme != INTERNET_SCHEME_HTTP &&
      parts.nScheme != INTERNET_SCHEME_HTTPS) {
    result.error = "адрес должен быть http или https";
    return result;
  }

  const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
  std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
  if (parts.lpszExtraInfo != nullptr && parts.dwExtraInfoLength > 0) {
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
  }
  if (path.empty()) {
    path = L"/";
  }

  HttpHandles handles;
  handles.Session() =
      WinHttpOpen(L"virtual_dnd_tabletop", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  if (handles.Session() == nullptr) {
    // Старые Windows не знают AUTOMATIC_PROXY: пробуем системный прокси.
    handles.Session() =
        WinHttpOpen(L"virtual_dnd_tabletop", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (handles.Session() == nullptr) {
    result.error = "WinHttpOpen: " + ErrorText(GetLastError());
    return result;
  }
  // Соединение — 3 с, запрос — 10 с: пауза модема не должна вешать поток.
  WinHttpSetTimeouts(handles.Session(), 3000, 3000, 10000, 10000);

  handles.Connect() =
      WinHttpConnect(handles.Session(), host.c_str(), parts.nPort, 0);
  if (handles.Connect() == nullptr) {
    result.error = "WinHttpConnect: " + ErrorText(GetLastError());
    return result;
  }

  const DWORD flags =
      parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
  handles.Request() = WinHttpOpenRequest(
      handles.Connect(), L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
      WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
  if (handles.Request() == nullptr) {
    result.error = "WinHttpOpenRequest: " + ErrorText(GetLastError());
    return result;
  }

  DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  WinHttpSetOption(handles.Request(), WINHTTP_OPTION_REDIRECT_POLICY, &policy,
                   sizeof(policy));
  WinHttpAddRequestHeaders(
      handles.Request(), L"User-Agent: virtual_dnd_tabletop\r\n",
      static_cast<DWORD>(-1),
      WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

  if (WinHttpSendRequest(handles.Request(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                         WINHTTP_NO_REQUEST_DATA, 0, 0, 0) == FALSE) {
    result.error = "нет связи: " + ErrorText(GetLastError());
    return result;
  }
  if (WinHttpReceiveResponse(handles.Request(), nullptr) == FALSE) {
    result.error = "нет ответа: " + ErrorText(GetLastError());
    return result;
  }

  DWORD status = 0;
  DWORD status_size = sizeof(status);
  if (WinHttpQueryHeaders(handles.Request(),
                          WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                          WINHTTP_NO_HEADER_INDEX) == FALSE) {
    result.error = "нет кода ответа: " + ErrorText(GetLastError());
    return result;
  }
  result.status = static_cast<int>(status);
  if (status != 200) {
    result.error = "HTTP " + std::to_string(status);
    return result;
  }

  std::uint64_t total = 0;
  DWORD length = 0;
  DWORD length_size = sizeof(length);
  if (WinHttpQueryHeaders(
          handles.Request(),
          WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
          WINHTTP_HEADER_NAME_BY_INDEX, &length, &length_size,
          WINHTTP_NO_HEADER_INDEX) != FALSE) {
    total = length;
  }
  if (total > max_bytes) {
    result.error = "ответ больше лимита";
    return result;
  }

  std::string body;
  if (total > 0) {
    body.reserve(static_cast<std::size_t>(total));
  }
  std::array<char, std::size_t{16} * 1024> buffer{};
  while (true) {
    DWORD read = 0;
    if (WinHttpReadData(handles.Request(), buffer.data(),
                        static_cast<DWORD>(buffer.size()), &read) == FALSE) {
      result.error = "обрыв загрузки: " + ErrorText(GetLastError());
      return result;
    }
    if (read == 0) {
      break;
    }
    if (body.size() + read > max_bytes) {
      result.error = "ответ больше лимита";
      return result;
    }
    body.append(buffer.data(), read);
    if (progress) {
      progress(body.size(), static_cast<std::size_t>(total));
    }
  }
  result.body = std::move(body);
  result.ok = true;
  return result;
}

#else

struct HttpResult {
  bool ok = false;
  int status = 0;
  std::string body;
  std::string error;
};

auto HttpGet(std::string_view /*url*/, std::size_t /*max_bytes*/,
             const std::function<void(std::size_t, std::size_t)>& /*progress*/)
    -> HttpResult {
  HttpResult result;
  result.error = "автообновление поддерживается только в Windows";
  return result;
}

#endif

}  // namespace

UpdateService::UpdateService(std::filesystem::path base_path, Version current,
                             int protocol, bool enabled,
                             std::string manifest_override)
    : base_path_(std::move(base_path)),
      manifest_override_(std::move(manifest_override)),
      current_(current),
      protocol_(protocol),
      enabled_(enabled) {}

UpdateService::~UpdateService() { JoinWorker(); }

auto UpdateService::Status() const -> UpdateStatus {
  const std::scoped_lock lock(mutex_);
  return status_;
}

auto UpdateService::BasePath() const -> const std::filesystem::path& {
  return base_path_;
}

auto UpdateService::JoinWorker() -> void {
  if (worker_.joinable()) {
    worker_.join();
  }
}

auto UpdateService::SetStage(UpdateStage stage, std::string error) -> void {
  const std::scoped_lock lock(mutex_);
  status_.stage = stage;
  status_.error = std::move(error);
}

auto UpdateService::SetFileProgress(int done, int total, int percent) -> void {
  const std::scoped_lock lock(mutex_);
  status_.files_done = done;
  status_.files_total = total;
  if (status_.progress != percent) {
    status_.progress = percent;
  }
}

auto UpdateService::StartCheck() -> void {
  if (!enabled_) {
    SDL_Log("update check is disabled (developer build or --no-update-check)");
    return;
  }
  SDL_Log("update check started (manifest: %s)",
          manifest_override_.empty() ? std::string(kUpdateManifestUrl).c_str()
                                     : manifest_override_.c_str());
  {
    const std::scoped_lock lock(mutex_);
    if (status_.stage == UpdateStage::kChecking ||
        status_.stage == UpdateStage::kDownloading) {
      return;
    }
    status_.stage = UpdateStage::kChecking;
    status_.error.clear();
    status_.progress = 0;
    status_.files_done = 0;
    status_.files_total = 0;
    status_.manual_only = false;
  }
  JoinWorker();
  worker_ = std::thread([this]() -> void { RunCheck(); });
}

auto UpdateService::StartDownload() -> void {
  if (!enabled_) {
    return;
  }
  {
    const std::scoped_lock lock(mutex_);
    if (status_.stage != UpdateStage::kUpdateAvailable &&
        status_.stage != UpdateStage::kFailed) {
      return;
    }
    if (status_.manifest.url.empty()) {
      return;
    }
    status_.stage = UpdateStage::kDownloading;
    status_.progress = 0;
    status_.files_done = 0;
    status_.files_total = 0;
    status_.error.clear();
  }
  JoinWorker();
  worker_ = std::thread([this]() -> void { RunDownload(); });
}

auto UpdateService::MarkApplying() -> void { SetStage(UpdateStage::kApplying); }

auto UpdateService::Fail(std::string error) -> void {
  SetStage(UpdateStage::kFailed, std::move(error));
}

auto UpdateService::RunCheck() -> void {
  std::string text;
  std::string error;

  if (manifest_override_.empty()) {
    const HttpResult result =
        HttpGet(kUpdateManifestUrl, kMaxUpdateManifestBytes, nullptr);
    if (!result.ok) {
      SDL_Log("update check skipped: %s", result.error.c_str());
      SetStage(UpdateStage::kIdle);
      return;
    }
    text = result.body;
    manifest_directory_ = base_path_;
  } else if (IsRemoteUrl(manifest_override_)) {
    const HttpResult result =
        HttpGet(manifest_override_, kMaxUpdateManifestBytes, nullptr);
    if (!result.ok) {
      SDL_Log("update check from %s failed: %s", manifest_override_.c_str(),
              result.error.c_str());
      SetStage(UpdateStage::kIdle);
      return;
    }
    text = result.body;
    manifest_directory_ = base_path_;
  } else {
    std::filesystem::path path = PathFromUtf8(manifest_override_);
    if (path.is_relative()) {
      path = base_path_ / path;
    }
    if (!ReadFileBytes(path, kMaxUpdateManifestBytes, text, error)) {
      SDL_Log("update manifest %s: %s", path.string().c_str(), error.c_str());
      SetStage(UpdateStage::kIdle);
      return;
    }
    manifest_directory_ = path.parent_path();
  }

  UpdateManifest manifest;
  if (!ParseUpdateManifest(text, manifest)) {
    SDL_Log("update manifest is malformed");
    SetStage(UpdateStage::kIdle);
    return;
  }
  if (!IsUpdateAvailable(current_, manifest, !manifest_override_.empty())) {
    SDL_Log("no newer version than %s (latest %s)",
            FormatVersion(current_).c_str(),
            FormatVersion(manifest.version).c_str());
    SetStage(UpdateStage::kIdle);
    return;
  }

  const bool writable = CanWriteDirectory(base_path_);
  {
    const std::scoped_lock lock(mutex_);
    status_.manifest = manifest;
    status_.manual_only = !writable;
    status_.progress = 0;
    status_.error.clear();
    status_.stage = UpdateStage::kUpdateAvailable;
  }
  SDL_Log(
      "update %s is available (protocol %d, current %d, version %s), "
      "manual only: %d",
      FormatVersion(manifest.version).c_str(), manifest.protocol, protocol_,
      FormatVersion(current_).c_str(), writable ? 0 : 1);
}

auto UpdateService::FetchFile(std::string_view url, std::size_t max_bytes,
                              int files_done, int files_total, std::string& out,
                              std::string& error) -> bool {
  out.clear();
  if (IsRemoteUrl(url)) {
    const HttpResult result =
        HttpGet(url, max_bytes,
                [this, files_done, files_total](std::size_t received,
                                                std::size_t file_size) -> void {
                  if (file_size == 0 || files_total <= 0) {
                    return;
                  }
                  const auto part = static_cast<int>(
                      std::min<std::size_t>(100, received * 100 / file_size));
                  SetFileProgress(files_done, files_total,
                                  (files_done * 100 + part) / files_total);
                });
    if (!result.ok) {
      error = result.error;
      return false;
    }
    out = result.body;
    return true;
  }
  // Офлайн-проверка цикла: ссылка — путь рядом с манифестом.
  std::filesystem::path path = PathFromUtf8(url);
  if (path.is_relative()) {
    const std::filesystem::path directory =
        manifest_directory_.empty() ? base_path_ : manifest_directory_;
    path = directory / path;
  }
  if (!ReadFileBytes(path, max_bytes, out, error)) {
    return false;
  }
  SetFileProgress(files_done, files_total,
                  ((files_done + 1) * 100) / std::max(files_total, 1));
  return true;
}

auto UpdateService::RunDownload() -> void {
  UpdateManifest manifest;
  {
    const std::scoped_lock lock(mutex_);
    manifest = status_.manifest;
  }
  if (manifest.url.empty() || manifest.sha256.empty()) {
    SetStage(UpdateStage::kFailed, "В манифесте нет ссылки или хеша");
    return;
  }

  // Ассеты, которых нет на диске или которые изменились. Совпавшие файлы не
  // качаем: при обычном обновлении скачивается только exe.
  UpdateDownloadPlan plan;
  PlanAssetDownloads(base_path_, manifest.files, plan);
  const auto files_total = static_cast<int>(1 + plan.files.size());
  SDL_Log("update download: %d file(s), %d asset(s), %llu bytes to fetch",
          files_total, static_cast<int>(plan.files.size()),
          static_cast<unsigned long long>(plan.total_bytes));

  std::string bytes;
  std::string error;
  if (!FetchFile(manifest.url, kMaxUpdateBytes, 0, files_total, bytes, error)) {
    SDL_Log("update download failed: %s", error.c_str());
    SetStage(UpdateStage::kFailed, "Не удалось скачать обновление: " + error);
    return;
  }

  if (HexDigest(Sha256(std::string_view(bytes))) != manifest.sha256) {
    SDL_Log("update sha256 mismatch for %s", manifest.url.c_str());
    SetStage(UpdateStage::kFailed,
             "Файл обновления повреждён (не совпал sha256)");
    return;
  }
  if (bytes.empty() || bytes.size() > kMaxUpdateBytes) {
    SetStage(UpdateStage::kFailed, "Файл обновления неверного размера");
    return;
  }

  const std::filesystem::path target = base_path_ / std::string(kUpdateExeName);
  if (!WriteFileBytes(target, bytes, error)) {
    SDL_Log("update write %s: %s", target.string().c_str(), error.c_str());
    SetStage(UpdateStage::kFailed, "Не удалось записать обновление: " + error);
    return;
  }
  SetFileProgress(1, files_total, 100 / files_total);

  // Ассеты кладём рядом с местом назначения: меняет файлы игры только
  // подмена, поэтому до нажатия «Перезапустить» папка остаётся рабочей.
  int done = 1;
  for (const UpdateFile& file : plan.files) {
    if (!FetchFile(file.url, kMaxUpdateBytes, done, files_total, bytes,
                   error)) {
      SDL_Log("update asset %s: %s", file.path.c_str(), error.c_str());
      SetStage(UpdateStage::kFailed,
               "Не удалось скачать " + file.path + ": " + error);
      return;
    }
    if (bytes.size() != file.size ||
        HexDigest(Sha256(std::string_view(bytes))) != file.sha256) {
      SDL_Log("update asset %s is damaged", file.path.c_str());
      SetStage(UpdateStage::kFailed,
               "Файл " + file.path + " повреждён (не совпал sha256)");
      return;
    }
    const std::filesystem::path staged =
        StagedUpdatePath(base_path_, file.path);
    std::error_code code;
    std::filesystem::create_directories(staged.parent_path(), code);
    if (!WriteFileBytes(staged, bytes, error)) {
      SDL_Log("update write %s: %s", staged.string().c_str(), error.c_str());
      SetStage(UpdateStage::kFailed,
               "Не удалось записать " + file.path + ": " + error);
      return;
    }
    ++done;
    SetFileProgress(done, files_total, done * 100 / files_total);
  }

  SetFileProgress(files_total, files_total, 100);
  SetStage(UpdateStage::kReady);
  SDL_Log("update %s is downloaded (%d file(s))",
          FormatVersion(manifest.version).c_str(), files_total);
}

auto InstallDownloadedUpdate(const std::filesystem::path& base_path,
                             const std::vector<UpdateFile>& files,
                             std::string& error) -> bool {
#ifdef _WIN32
  const std::filesystem::path exe = base_path / std::string(kExecutableName);
  const std::filesystem::path previous =
      base_path / std::string(kPreviousExeName);
  const std::filesystem::path update = base_path / std::string(kUpdateExeName);
  std::error_code code;
  if (!std::filesystem::exists(update, code)) {
    error = "Скачанный файл обновления не найден";
    return false;
  }

  // Ассеты — до exe: если подмена сорвётся на файле, игра останется со старым
  // exe, а он новые файлы в assets/ просто не читает.
  for (const UpdateFile& file : files) {
    const std::filesystem::path staged = StagedUpdatePath(base_path, file.path);
    if (!std::filesystem::exists(staged, code)) {
      continue;
    }
    const std::filesystem::path asset =
        base_path / std::filesystem::path(file.path);
    std::filesystem::create_directories(asset.parent_path(), code);
    if (MoveFileExW(staged.wstring().c_str(), asset.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
        FALSE) {
      error = "Не удалось обновить " + file.path + ": " +
              ErrorText(GetLastError()) +
              ". Игра продолжает работать, нажмите «Повторить»";
      return false;
    }
  }

  // Старый остаток от прошлой подмены не должен мешать первому rename.
  std::filesystem::remove(previous, code);
  if (MoveFileExW(exe.wstring().c_str(), previous.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    error = "Не удалось подготовить подмену: " + ErrorText(GetLastError());
    return false;
  }
  if (MoveFileExW(update.wstring().c_str(), exe.wstring().c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) ==
      FALSE) {
    const std::string reason = ErrorText(GetLastError());
    // Антивирус мог задержать файл: возвращаем рабочий exe на место.
    const bool rolled_back =
        MoveFileExW(previous.wstring().c_str(), exe.wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) !=
        FALSE;
    error = rolled_back ? "Не удалось заменить exe (" + reason +
                              "). Прежняя версия восстановлена"
                        : "Не удалось ни заменить exe (" + reason +
                              "), ни вернуть прежний. Запустите " +
                              std::string(kExecutableName) + " из папки игры";
    return false;
  }

  // Именованный мьютекс отпускаем до запуска нового экземпляра: иначе он
  // увидит «игра уже запущена» и сразу выйдет.
  ReleaseInstanceLock();
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  const std::wstring command = exe.wstring();
  if (CreateProcessW(const_cast<LPWSTR>(command.c_str()), nullptr, nullptr,
                     nullptr, FALSE, 0, nullptr, base_path.wstring().c_str(),
                     &startup, &process) == FALSE) {
    error = "Обновление установлено, но перезапуск не удался: " +
            ErrorText(GetLastError()) + ". Запустите игру вручную";
    // Подмена уже выполнена: выходить можно, новый exe на месте.
    return true;
  }
  CloseHandle(process.hThread);
  CloseHandle(process.hProcess);
  return true;
#else
  (void)base_path;
  (void)files;
  error = "автообновление поддерживается только в Windows";
  return false;
#endif
}

}  // namespace dnd
