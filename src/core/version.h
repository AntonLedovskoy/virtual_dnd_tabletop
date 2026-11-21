#pragma once

#include <string>
#include <string_view>

namespace dnd {

// Версия приходит из `git describe --tags --always --dirty` на момент
// configure: `v0.5.2` на самом теге, `v0.5.2-3-gabc123` после коммитов,
// `v0.5.2-3-gabc123-dirty` с правками в рабочей копии.
//
// dirty — сборка не ровно на чистом теге: изменения, коммиты после тега или
// отсутствие тега. Это developer build: автообновление ей выключается, иначе
// она предлагала бы обновиться сама на себя.
struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool dirty = false;
};

// Разбирает `0.5.2`, `v0.5.2`, `0.5.2-3-gabc123`, `0.5.2-3-gabc123-dirty` и
// `0.5.2-dirty`. Номер — ровно три неотрицательных числа через точку; любой
// непустой суффикс после первого дефиса означает developer build.
// При ошибке `version` не меняется.
[[nodiscard]] auto ParseVersion(std::string_view text, Version& version)
    -> bool;

// Сравнение по числам без учёта dirty: -1, 0 или 1.
[[nodiscard]] auto CompareVersion(const Version& left, const Version& right)
    -> int;

// "0.5.2" или "0.5.2-dirty".
[[nodiscard]] auto FormatVersion(const Version& version) -> std::string;

}  // namespace dnd
