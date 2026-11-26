#!/usr/bin/env bash
# Builds the mod (and libfpv for Windows i686) and copies it into Portal 2.
# Everything takes effect in a running game except the loader:
#
#   tools/deploy.sh          libfpv, the core, the content. A new libfpv goes to
#                            libfpv-live\ and replaces the running one when the
#                            new core reloads (src/fpvapi.cpp), within a second;
#                            portal2-fpv.log says so. The loader takes effect at
#                            the next game start.
#   tools/deploy.sh core     only core.dll (hot-reloaded)
#
# KAMIKAZE=1 (or in tools/deploy.local.sh, not in git) deploys a local
# development build against a kamikaze libfpv (libfpv's build/win32-kamikaze,
# the mod's build-kamikaze); never shipped, CI builds without. The default is
# kamikaze on a kamikaze dev machine (LIBFPV_KAMIKAZE_DEV=1 or true, libfpv
# CLAUDE.md), else the release flavour (libfpv's build/win32, the mod's build).
# The two must be of one kind: fpv_frame's layout differs.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
# macOS (Portal 2 recompiled by source-recomp): its own build and folders
[ "$(uname)" = Darwin ] && exec "$here/tools/deploy-mac.sh" "$@"
game="${PORTAL2_DIR:-/mnt/d/SteamLibrary/steamapps/common/Portal 2}"
libfpv="${LIBFPV_DIR:-$HOME/Projects/libfpv}"
dest="$game/portal2/addons/portal2-fpv"
mode="${1:-all}"
[ -f "$here/tools/deploy.local.sh" ] && . "$here/tools/deploy.local.sh"
case "${LIBFPV_KAMIKAZE_DEV:-}" in 1|true) dev_kamikaze=1 ;; *) dev_kamikaze=0 ;; esac
kamikaze="${KAMIKAZE:-$dev_kamikaze}"

if [ "$kamikaze" = 1 ]; then
  libbuild="$libfpv/build/win32-kamikaze" build="$here/build-kamikaze" kflag=ON
else
  libbuild="$libfpv/build/win32" build="$here/build" kflag=OFF
fi

if [ "$mode" != core ]; then
  [ -f "$libbuild/CMakeCache.txt" ] || cmake -S "$libfpv" -B "$libbuild" \
    -DCMAKE_TOOLCHAIN_FILE="$libfpv/cmake/toolchains/mingw-w64-i686.cmake" -DLIBFPV_KAMIKAZE=$kflag >/dev/null
  if ! out="$(cmake --build "$libbuild" -j"$(nproc)" --target fpv 2>&1)"; then
    echo "$out" | grep -E "error" -A3 >&2
    echo "libfpv build failed: nothing deployed" >&2
    exit 1
  fi
fi
[ -f "$build/CMakeCache.txt" ] || cmake -S "$here" -B "$build" -DCMAKE_TOOLCHAIN_FILE="$here/cmake/mingw-w64-i686.cmake" \
  -DCMAKE_BUILD_TYPE=Release -DLIBFPV_DIR="$libfpv" -DP2FPV_KAMIKAZE=$kflag >/dev/null
# a failed build stops here: never deploy the previous binaries as if they were new
if ! out="$(cmake --build "$build" -j"$(nproc)" 2>&1)"; then
  echo "$out" | grep -E "error" -A3 >&2
  echo "build failed: nothing deployed" >&2
  exit 1
fi
echo "$out" | grep -E "warning" || true
# the mod and libfpv of one kind, or the mod reads every frame shifted
mod_kflag="$(sed -n 's/^P2FPV_KAMIKAZE:BOOL=//p' "$build/CMakeCache.txt")"
lib_kflag=OFF
grep -q fpv_kamikaze_poll_detonation "$libbuild/libfpv.dll" && lib_kflag=ON
if [ "$mod_kflag" != "$kflag" ] || [ "$lib_kflag" != "$kflag" ]; then
  echo "kamikaze mismatch: wanted $kflag, the mod ($build) is $mod_kflag, libfpv ($libbuild) is $lib_kflag: nothing deployed" >&2
  echo "  cmake -S . -B $build -DP2FPV_KAMIKAZE=$kflag (and libfpv's -DLIBFPV_KAMIKAZE=$kflag)" >&2
  exit 1
fi

mkdir -p "$dest/fpv-data"
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
  # libfpv: the newest build there is what runs (or will); a different one
  # goes to libfpv-live\ for the running game, before the core that loads it
  newest="$(ls -t "$dest"/libfpv-live/libfpv-*.dll "$dest/libfpv.dll" 2>/dev/null | head -1 || true)"
  if [ -z "$newest" ] || ! cmp -s "$libbuild/libfpv.dll" "$newest"; then
    swap "$libbuild/libfpv.dll" "$dest/libfpv.dll" >/dev/null
    sleep 1  # the live copy is the newest file
    mkdir -p "$dest/libfpv-live"
    cp "$libbuild/libfpv.dll" "$dest/libfpv-live/libfpv-$(date +%s).dll"
    echo "libfpv: new build (the running game switches to it with the core)"
  fi
  swap "$build/portal2-fpv.dll" "$dest/portal2-fpv.dll"
  cp "$here/addons/portal2-fpv.vdf" "$game/portal2/addons/portal2-fpv.vdf"
  if [ -d "$here/content" ]; then cp -r "$here/content/." "$game/portal2/"; fi
fi
cp "$build/core.dll" "$dest/core.dll"
echo "deployed ($mode, kamikaze=$kamikaze) to $dest"
