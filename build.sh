#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

usage() {
  cat <<EOF
Использование: $(basename "$0") [параметры] [-- <аргументы сборки>...]

Параметры:
  -p, --profile <имя>   Профиль: debug (по умолчанию), release,
                        relwithdebinfo, minsize
  -c, --configure       Принудительно настроить CMake перед сборкой
  -j, --jobs <n>        Число параллельных задач (по умолчанию: число CPU)
  -h, --help            Показать эту справку

Каталог сборки: .build/<profile>

Примеры:
  ./build.sh
  ./build.sh -p release
  ./build.sh -p release -c
  ./build.sh -p debug -- --verbose
EOF
}

PROFILE=debug
FORCE_CONFIGURE=0
JOBS=""
EXTRA_ARGS=()

while [[ $# -gt 0 ]]; do
  case "$1" in
    -p|--profile)
      [[ $# -ge 2 ]] || { echo "error: $1 requires an argument" >&2; exit 1; }
      PROFILE=$2
      shift 2
      ;;
    --profile=*)
      PROFILE=${1#*=}
      shift
      ;;
    -c|--configure)
      FORCE_CONFIGURE=1
      shift
      ;;
    -j|--jobs)
      [[ $# -ge 2 ]] || { echo "error: $1 requires an argument" >&2; exit 1; }
      JOBS=$2
      shift 2
      ;;
    --jobs=*)
      JOBS=${1#*=}
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      EXTRA_ARGS=("$@")
      break
      ;;
    *)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

PROFILE=$(printf '%s' "$PROFILE" | tr '[:upper:]' '[:lower:]')
case "$PROFILE" in
  debug)
    PROFILE=debug
    BUILD_TYPE=Debug
    ;;
  release)
    PROFILE=release
    BUILD_TYPE=Release
    ;;
  relwithdebinfo|relwithdeb)
    PROFILE=relwithdebinfo
    BUILD_TYPE=RelWithDebInfo
    ;;
  minsize|minimal|min)
    PROFILE=minsize
    BUILD_TYPE=MinSizeRel
    ;;
  *)
    echo "error: unknown profile '$PROFILE'" >&2
    echo "       supported: debug, release, relwithdebinfo, minsize" >&2
    exit 1
    ;;
esac

if [[ -z "$JOBS" ]]; then
  JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo "${NUMBER_OF_PROCESSORS:-1}")
fi
[[ "$JOBS" =~ ^[0-9]+$ && "$JOBS" -gt 0 ]] || {
  echo "error: invalid job count '$JOBS'" >&2
  exit 1
}

GENERATOR=()
if command -v ninja >/dev/null 2>&1; then
  GENERATOR=(-G Ninja)
fi

# FetchContent клонирует репозитории зависимостей, поэтому cmake нужен git.
# В shell MSYS2 он есть не всегда; используем отдельный Git for Windows.
GIT_HINT=()
if ! command -v git >/dev/null 2>&1; then
  for candidate in "/c/Program Files/Git/cmd/git.exe" "/c/Program Files (x86)/Git/cmd/git.exe"; do
    if [[ -x "$candidate" ]]; then
      GIT_HINT=(-DGIT_EXECUTABLE="$(cygpath -m "$candidate")")
      break
    fi
  done
  if [[ ${#GIT_HINT[@]} -eq 0 ]]; then
    echo "warning: git not found; CMake FetchContent will fail to download dependencies" >&2
  fi
fi

BUILD_DIR="$SCRIPT_DIR/.build/$PROFILE"
CACHE="$BUILD_DIR/CMakeCache.txt"

configure() {
  echo "==> Configuring '$PROFILE' ($BUILD_TYPE) in .build/$PROFILE"
  cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
        -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
        ${GENERATOR[@]+"${GENERATOR[@]}"} \
        ${GIT_HINT[@]+"${GIT_HINT[@]}"}
}

update_compile_db() {
  local source="$BUILD_DIR/compile_commands.json"
  local target="$SCRIPT_DIR/compile_commands.json"
  local relative=".build/$PROFILE/compile_commands.json"
  [[ -f "$source" ]] || return 0
  rm -f "$target"
  MSYS=winsymlinks:nativestrict ln -s "$relative" "$target" 2>/dev/null || cp -f "$source" "$target"
}

cache_matches_sources() {
  local cached
  cached=$(sed -n 's/^CMAKE_HOME_DIRECTORY:INTERNAL=//p' "$CACHE" | head -n 1)
  [[ -n "$cached" ]] || return 1
  local here there
  here=$(cd "$SCRIPT_DIR" && pwd -W 2>/dev/null || pwd)
  there=$(cd "$cached" 2>/dev/null && pwd -W 2>/dev/null || printf '%s' "$cached")
  [[ "$here" == "$there" ]]
}

is_configured() {
  [[ -f "$CACHE" ]] || return 1
  cache_matches_sources || return 1
  [[ -f "$BUILD_DIR/compile_commands.json" ]] || return 1
  [[ -f "$BUILD_DIR/build.ninja" || -f "$BUILD_DIR/Makefile" ]] && return 0
  compgen -G "$BUILD_DIR/*.sln" >/dev/null
}

if (( FORCE_CONFIGURE )); then
  echo "==> Configure forced"
  configure
elif is_configured; then
  echo "==> Already configured in .build/$PROFILE, skipping configure (use -c to force)"
else
  [[ -f "$CACHE" ]] && echo "==> Existing configuration is incomplete or stale, starting over"
  rm -rf "$BUILD_DIR"
  configure
fi

update_compile_db
echo "==> compile_commands.json -> .build/$PROFILE/compile_commands.json"

echo "==> Building '$PROFILE' with $JOBS job(s)"
cmake --build "$BUILD_DIR" --config "$BUILD_TYPE" --parallel "$JOBS" \
      ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}

# CMake мог перегенерировать compile_commands.json во время сборки.
update_compile_db

echo "==> Done: .build/$PROFILE"
