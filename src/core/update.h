#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/version.h"

namespace dnd {

// Файл релиза рядом с exe: шрифт, лицензия, будущие звуки и картинки. Путь
// назначения — только внутри `assets/`: манифест не должен уметь переписать
// exe, кампанию или журнал. `size` нужен для прогресса и проверки длины.
struct UpdateFile {
  std::string path;
  std::string url;
  std::string sha256;
  std::uint64_t size = 0;
};

// Манифест `latest.json` из последнего релиза. Поля, которых в файле нет,
// остаются пустыми: обязательны только version, url и sha256 — без них
// обновляться нечем. `files` в старых манифестах нет: тогда обновляется
// только exe.
struct UpdateManifest {
  Version version;
  int protocol = 0;
  std::string url;
  // 64 шестнадцатеричных символа в нижнем регистре.
  std::string sha256;
  std::string notes;
  std::string published;
  std::vector<UpdateFile> files;
};

// Лимиты, общие для сети и файлов: манифест tiny, exe — десятки мегабайт.
// Больше не читаем, чтобы чужая ссылка не съела память игрока.
inline constexpr std::size_t kMaxUpdateManifestBytes = std::size_t{64} * 1024;
inline constexpr std::size_t kMaxUpdateBytes = std::size_t{64} * 1024 * 1024;
// Ассетов немного (шрифт, лицензии), но лимит нужен: манифест чужой.
inline constexpr std::size_t kMaxUpdateFiles = 64;
inline constexpr std::size_t kMaxUpdateTotalBytes =
    std::size_t{128} * 1024 * 1024;

// Ассеты живут только в `assets/`: так манифест не может тронуть данные
// стола. Временная копия скачанного файла — рядом с местом назначения с этим
// суффиксом; такие имена в манифесте запрещены.
inline constexpr std::string_view kAssetDirectory = "assets/";
inline constexpr std::string_view kStagedSuffix = ".update";

// Стабильная ссылка на манифест последнего релиза: без GitHub API, лимитов и
// токена. Подменяется через DND_UPDATE_MANIFEST при отладке.
inline constexpr std::string_view kUpdateManifestUrl =
    "https://github.com/AntonLedovskoy/virtual_dnd_tabletop/releases/latest/"
    "download/latest.json";

// Страница всех релизов: сюда ведёт диалог несовпадения версий, когда
// последнюю версию узнать не удалось.
inline constexpr std::string_view kReleasesUrl =
    "https://github.com/AntonLedovskoy/virtual_dnd_tabletop/releases";

// Имя скачанного бинарника рядом с текущим exe и имя прежнего exe на время
// подмены. Оба начинаются с имени игры: `virtual_dnd_tabletop`.
inline constexpr std::string_view kExecutableName = "virtual_dnd_tabletop.exe";
inline constexpr std::string_view kUpdateExeName =
    "virtual_dnd_tabletop.update.exe";
inline constexpr std::string_view kPreviousExeName =
    "virtual_dnd_tabletop.old.exe";

// Терпимый карликовый JSON-ридер верхнего уровня: берёт строки version, url,
// sha256, notes, published и число protocol, остальные ключи и вложенные
// значения пропускает. Неизвестные поля не ошибка, битая структура — ошибка.
// При ошибке manifest не меняется.
[[nodiscard]] auto ParseUpdateManifest(std::string_view json,
                                       UpdateManifest& manifest) -> bool;

// Путь ассета из манифеста: относительный, только `assets/…`, сегменты без
// `..`, `:` и обратных слэшей, без хвостовой точки (Windows её отбрасывает) и
// без нашего временного суффикса. Всё остальное — битый манифест.
[[nodiscard]] auto IsSafeUpdatePath(std::string_view path) -> bool;

// Куда качать файл до подмены: `<base>/<path>.update`, рядом с местом
// назначения, чтобы перенос был переименованием в пределах тома.
[[nodiscard]] auto StagedUpdatePath(const std::filesystem::path& base,
                                    std::string_view path)
    -> std::filesystem::path;

// Что из манифеста действительно нужно скачать: файла нет или его sha256 не
// совпал. Сумма размеров — для прогресса.
struct UpdateDownloadPlan {
  std::vector<UpdateFile> files;
  std::uint64_t total_bytes = 0;
};

auto PlanAssetDownloads(const std::filesystem::path& base,
                        const std::vector<UpdateFile>& files,
                        UpdateDownloadPlan& plan) -> void;

// Убирает недокачанные ассеты (`*.update` под `assets/`), оставшиеся после
// обрыва между скачиванием и подменой. Ошибки не важны: файл уберётся при
// следующем старте.
auto RemoveStagedAssets(const std::filesystem::path& base) -> void;

// Предлагать ли обновление: текущая сборка — релиз (не developer build), а
// версия в манифесте новее. Протокол не сравнивается: версия важнее, а
// несовпадение версий лечится обновлением. allow_developer_build — отладочный
// обход через DND_UPDATE_MANIFEST: офлайн проверяется весь цикл подмены.
[[nodiscard]] auto IsUpdateAvailable(const Version& current,
                                     const UpdateManifest& manifest,
                                     bool allow_developer_build = false)
    -> bool;

// Вердикт для диалога несовпадения версий: кто отстал и поможет ли
// обновление. Последняя версия известна только после проверки манифеста:
// пусто — сети не было, и тогда советовать нечего, остаётся ссылка на релизы.
enum class UpdateAdvice : std::uint8_t {
  kUnknown = 0,
  // Моя сборка старше хоста: обновление помогает.
  kUpdateMine = 1,
  // Хост старше: обновиться должен DM, клиенту это не поможет.
  kUpdateHost = 2,
  // Версии совпали, но в манифесте есть новее: игру не блокирует.
  kNewerAvailable = 3,
  // Версии совпали и новее нет: протоколы различаются только сборками.
  kUpToDate = 4,
};

[[nodiscard]] auto AdviseUpdate(const Version& mine, const Version& host,
                                const std::optional<Version>& latest)
    -> UpdateAdvice;

// Что делать на старте с файлами подмены, если прошлый запуск прервался между
// шагами. exe всегда есть у запущенного процесса, остальные случаи — защита и
// предмет теста: обрыв не должен оставлять «игра не запускается».
enum class StartupAction : std::uint8_t {
  kNone = 0,
  // `old.exe` — единственный целый бинарник: вернуть его на место.
  kRestoreOld = 1,
  // exe пропал, но скачанный `update.exe` на месте: доделать подмену.
  kFinishSwap = 2,
  // Подмена завершена: убрать `old.exe` и недокачанный `update.exe`.
  kCleanup = 3,
};

[[nodiscard]] auto PlanStartupRecovery(bool have_exe, bool have_old,
                                       bool have_update) -> StartupAction;

// Выполняет действие из PlanStartupRecovery. Ошибки не роняют запуск: App
// пишет их в лог, игра продолжает работать на том, что есть.
[[nodiscard]] auto ApplyStartupRecovery(const std::filesystem::path& directory,
                                        StartupAction action,
                                        std::string& error) -> bool;

}  // namespace dnd
