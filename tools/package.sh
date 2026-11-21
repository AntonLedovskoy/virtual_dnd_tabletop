#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT_DIR=$(cd "$SCRIPT_DIR/.." && pwd)
BUILD_DIR="$ROOT_DIR/.build/release"
DIST_DIR="$ROOT_DIR/dist"

usage() {
  cat <<EOF
Использование: $(basename "$0") [параметры]

Собирает релиз с текущего тега vX.Y.Z: release-сборку, портативный zip и
latest.json в dist/.

Параметры:
  -j, --jobs <n>   Число параллельных задач (по умолчанию: число CPU)
  --allow-dev      Разрешить сборку не с чистого тега (проверка пайплайна,
                   такой архив нельзя выкладывать)
  --skip-build     Не пересобирать, взять готовый
                   .build/release/virtual_dnd_tabletop.exe
  -h, --help       Показать эту справку

Примеры:
  ./tools/package.sh
  ./tools/package.sh -j 8
  ./tools/package.sh --allow-dev --skip-build
EOF
}

ALLOW_DEV=0
SKIP_BUILD=0
JOBS=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    -j|--jobs)
      [[ $# -ge 2 ]] || { echo "error: $1 requires an argument" >&2; exit 1; }
      JOBS=$2
      shift 2
      ;;
    --jobs=*)
      JOBS=${1#*=}
      shift
      ;;
    --allow-dev)
      ALLOW_DEV=1
      shift
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "error: unknown option: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -n "$JOBS" ]]; then
  [[ "$JOBS" =~ ^[0-9]+$ && "$JOBS" -gt 0 ]] || {
    echo "error: invalid job count '$JOBS'" >&2
    exit 1
  }
fi

cd "$ROOT_DIR"

PACKAGE=virtual_dnd_tabletop
EXE_NAME=$PACKAGE.exe

# git нужен для версии и заметок; в MSYS2 его может не быть, тогда берём
# Git for Windows — так же, как build.sh подсказывает путь CMake.
GIT_BIN=$(command -v git 2>/dev/null || true)
if [[ -z "$GIT_BIN" ]]; then
  for candidate in "/c/Program Files/Git/cmd/git.exe" \
    "/c/Program Files (x86)/Git/cmd/git.exe"; do
    if [[ -x "$candidate" ]]; then
      GIT_BIN=$candidate
      break
    fi
  done
fi
[[ -n "$GIT_BIN" ]] || {
  echo "error: git не найден; он нужен для версии и заметок релиза" >&2
  exit 1
}

# Версия — только с тега: номер в манифесте и в бинарнике должен совпадать с
# тем, что видят игроки. --allow-dev нужен для проверки всего цикла до релиза.
if ! DESCRIBE=$("$GIT_BIN" describe --tags --always --dirty 2>/dev/null); then
  echo "error: git describe не сработал; релиз собирается из git-репозитория" >&2
  exit 1
fi
if [[ "$DESCRIBE" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
  VERSION=${BASH_REMATCH[1]}
  TAG="v$VERSION"
elif (( ALLOW_DEV )) && [[ "$DESCRIBE" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)(-|$) ]]; then
  VERSION=${BASH_REMATCH[1]}
  TAG=""
  echo "warning: сборка не с чистого тега ('$DESCRIBE'): только для проверки" >&2
else
  echo "error: релиз собирается только с чистого тега vX.Y.Z" >&2
  echo "       сейчас git describe = '$DESCRIBE'" >&2
  echo "       для локальной проверки: --allow-dev" >&2
  exit 1
fi

ZIP_NAME=$PACKAGE-$VERSION-win64.zip
STAGE_NAME=$PACKAGE-$VERSION

ORIGIN=$("$GIT_BIN" remote get-url origin 2>/dev/null || true)
case "$ORIGIN" in
  git@github.com:*)
    REPO_URL="https://github.com/${ORIGIN#git@github.com:}"
    ;;
  ssh://git@github.com/*)
    REPO_URL="https://github.com/${ORIGIN#ssh://git@github.com/}"
    ;;
  https://*)
    REPO_URL=$ORIGIN
    ;;
  *)
    echo "error: origin должен смотреть на GitHub, сейчас '$ORIGIN'" >&2
    exit 1
    ;;
esac
REPO_URL=${REPO_URL%.git}

PROTOCOL=$(sed -n 's/.*kProtocolVersion = \([0-9][0-9]*\).*/\1/p' \
  src/net/protocol.h | head -n 1)
[[ -n "$PROTOCOL" ]] || {
  echo "error: не удалось прочитать kProtocolVersion из src/net/protocol.h" >&2
  exit 1
}

EXE="$BUILD_DIR/$EXE_NAME"
if (( SKIP_BUILD )); then
  [[ -f "$EXE" ]] || {
    echo "error: $EXE не найден; уберите --skip-build" >&2
    exit 1
  }
else
  # -c обязателен: DND_VERSION фиксируется на configure, а тег появился позже.
  if [[ -n "$JOBS" ]]; then
    "$ROOT_DIR/build.sh" -p release -c -j "$JOBS"
  else
    "$ROOT_DIR/build.sh" -p release -c
  fi
fi

# Бинарник должен нести версию того тега, с которого собираем манифест.
EXPECTED=$DESCRIBE
[[ -n "$TAG" ]] && EXPECTED=$TAG
if [[ -f "$BUILD_DIR/build.ninja" ]] &&
  ! grep -qF "DND_VERSION=\\\"$EXPECTED\\\"" "$BUILD_DIR/build.ninja"; then
  echo "error: exe собран с другой версией; запустите без --skip-build" >&2
  exit 1
fi

NOTES=$("$GIT_BIN" for-each-ref "refs/tags/${TAG:-v$VERSION}" \
  --format='%(contents)')
if [[ -z "$NOTES" ]]; then
  NOTES=$("$GIT_BIN" log -1 --format='%s')
fi

json_escape() {
  local text=$1
  text=${text//\\/\\\\}
  text=${text//\"/\\\"}
  text=${text//$'\r'/}
  text=${text//$'\n'/\\n}
  text=${text//$'\t'/\\t}
  printf '%s' "$text"
}

STAGE="$DIST_DIR/$STAGE_NAME"
rm -rf "$STAGE"
rm -f "$DIST_DIR/$EXE_NAME" "$DIST_DIR/$ZIP_NAME" \
  "$DIST_DIR/latest.json" "$DIST_DIR/notes.txt"
mkdir -p "$STAGE"
cp "$EXE" "$STAGE/$EXE_NAME"
# Шрифт кладём в архив, если он есть в репозитории: без него игра возьмёт
# системный, но тогда вёрстка у игроков будет разной.
FONT="$ROOT_DIR/assets/fonts/DejaVuSans.ttf"
if [[ -f "$FONT" ]]; then
  mkdir -p "$STAGE/assets/fonts"
  cp "$FONT" "$STAGE/assets/fonts/DejaVuSans.ttf"
else
  echo "warning: $FONT не найден, zip соберётся без шрифта" >&2
fi
cp "$EXE" "$DIST_DIR/$EXE_NAME"

# zip без внешнего пакета: cmake -E tar есть в тех же UCRT64-инструментах.
(cd "$STAGE" &&
  cmake -E tar cf "$(cygpath -m "$DIST_DIR/$ZIP_NAME")" --format=zip .)

SHA256=$(sha256sum "$DIST_DIR/$EXE_NAME" | cut -d' ' -f1)
PUBLISHED=$(date -u +%Y-%m-%d)
printf '%s\n' "$NOTES" >"$DIST_DIR/notes.txt"
cat >"$DIST_DIR/latest.json" <<EOF
{
  "version": "$VERSION",
  "protocol": $PROTOCOL,
  "url": "$REPO_URL/releases/download/v$VERSION/$EXE_NAME",
  "sha256": "$SHA256",
  "notes": "$(json_escape "$NOTES")",
  "published": "$PUBLISHED"
}
EOF

echo "==> dist/$STAGE_NAME/$EXE_NAME"
echo "==> dist/$ZIP_NAME"
echo "==> dist/latest.json (version $VERSION, protocol $PROTOCOL)"
echo "==> sha256 $SHA256"
if [[ -n "$TAG" ]]; then
  echo "==> Загрузка:"
  echo "    gh release create $TAG dist/$EXE_NAME \\"
  echo "      dist/$ZIP_NAME dist/latest.json \\"
  echo "      --title \"$PACKAGE $VERSION\" --notes-file dist/notes.txt"
fi
