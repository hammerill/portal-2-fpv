#!/usr/bin/env bash
# Builds the mod (and libfpv for Windows i686) and copies it into Portal 2.
#
#   tools/deploy.sh          everything (with the game running, the loader and
#                            libfpv.dll take effect at its next start)
#   tools/deploy.sh core     only core.dll: the running game hot-reloads it
#                            within a second (fpv-data/../portal2-fpv.log says so)
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
game="${PORTAL2_DIR:-/mnt/d/SteamLibrary/steamapps/common/Portal 2}"
libfpv="${LIBFPV_DIR:-$HOME/Projects/libfpv}"
dest="$game/portal2/addons/portal2-fpv"
mode="${1:-all}"

if [ "$mode" != core ]; then
  cmake --build "$libfpv/build/win32" -j"$(nproc)" --target fpv >/dev/null
fi
[ -d "$here/build" ] || cmake -S "$here" -B "$here/build" -DCMAKE_TOOLCHAIN_FILE="$here/cmake/mingw-w64-i686.cmake" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$here/build" -j"$(nproc)" 2>&1 | grep -E "error|warning" || true

mkdir -p "$dest/fpv-data"
cp "$here/build/core.dll" "$dest/core.dll"
# A running game locks the loader and libfpv, but Windows lets a loaded DLL be
# renamed: the new one goes in beside it and loads at the next start.
swap() {
  rm -f "$2.old" 2>/dev/null || true
  if ! cp "$1" "$2" 2>/dev/null; then
    mv "$2" "$2.old" && cp "$1" "$2"
    echo "$(basename "$2") is in use: replaced, takes effect when the game restarts"
  fi
}
if [ "$mode" != core ]; then
  swap "$here/build/portal2-fpv.dll" "$dest/portal2-fpv.dll"
  swap "$libfpv/build/win32/libfpv.dll" "$dest/libfpv.dll"
  cp "$here/addons/portal2-fpv.vdf" "$game/portal2/addons/portal2-fpv.vdf"
  if [ -d "$here/content" ]; then cp -r "$here/content/." "$game/portal2/"; fi
fi
echo "deployed ($mode) to $dest"
