#!/usr/bin/env bash
# macOS: builds the mod (and libfpv) for Portal 2 recompiled by source-recomp
# (an arm64 app running the Linux build) and copies it into the Steam game
# folder, which the app reads. Same modes as tools/deploy.sh:
#
#   tools/deploy-mac.sh          libfpv, the core, the plugin, the content. A
#                                new libfpv goes to libfpv-live/ and replaces
#                                the running one when the new core reloads.
#   tools/deploy-mac.sh core     only core.dylib (hot-reloaded within a second)
#
# The game needs a source-recomp with native plugins (its runtime loads
# addons/portal2-fpv/portal2-fpv.so, an arm64 dylib, in place of a Linux
# module); docs/INSTALL.md says which.
#
# KAMIKAZE=1 (the default here, or 0 in tools/deploy.local.sh) deploys a local
# development build against a kamikaze libfpv (libfpv's build/mac-kamikaze,
# the mod's build/mac-kamikaze); never shipped.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
game="${PORTAL2_DIR:-$HOME/Library/Application Support/Steam/steamapps/common/Portal 2}"
libfpv="${LIBFPV_DIR:-$HOME/Projects/libfpv}"
dest="$game/portal2/addons/portal2-fpv"
mode="${1:-all}"
[ -f "$here/tools/deploy.local.sh" ] && . "$here/tools/deploy.local.sh"
kamikaze="${KAMIKAZE:-1}"
jobs="$(sysctl -n hw.ncpu)"

if [ "$kamikaze" = 1 ]; then
  libbuild="$libfpv/build/mac-kamikaze" build="$here/build/mac-kamikaze" kflag=ON
else
  libbuild="$libfpv/build/mac" build="$here/build/mac" kflag=OFF
fi

if [ "$mode" != core ]; then
  [ -f "$libbuild/CMakeCache.txt" ] || cmake -S "$libfpv" -B "$libbuild" -DCMAKE_BUILD_TYPE=Release \
    -DLIBFPV_KAMIKAZE=$kflag >/dev/null
  if ! out="$(cmake --build "$libbuild" -j"$jobs" --target fpv 2>&1)"; then
    echo "$out" | grep -E "error" -A3 >&2
    echo "libfpv build failed: nothing deployed" >&2
    exit 1
  fi
fi
[ -f "$build/CMakeCache.txt" ] || cmake -S "$here" -B "$build" -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DLIBFPV_DIR="$libfpv" -DLIBFPV_BUILD="$libbuild" -DP2FPV_KAMIKAZE=$kflag >/dev/null
# a failed build stops here: never deploy the previous binaries as if they were new
if ! out="$(cmake --build "$build" -j"$jobs" 2>&1)"; then
  echo "$out" | grep -E "error" -A3 >&2
  echo "build failed: nothing deployed" >&2
  exit 1
fi
echo "$out" | grep -E "warning" || true

mkdir -p "$dest/fpv-data"
# A loaded dylib may be replaced on disk (a new file, not overwritten in
# place: the running copy stays mapped); it loads at the next start.
put() { rm -f "$2.new"; cp -L "$1" "$2.new" && mv -f "$2.new" "$2"; }
if [ "$mode" != core ]; then
  lib="$libbuild/libfpv.dylib"
  newest="$(ls -t "$dest"/libfpv-live/libfpv-*.dylib "$dest/libfpv.dylib" 2>/dev/null | head -1 || true)"
  if [ -z "$newest" ] || ! cmp -s "$lib" "$newest"; then
    put "$lib" "$dest/libfpv.dylib"
    sleep 1  # the live copy is the newest file
    mkdir -p "$dest/libfpv-live"
    cp -L "$lib" "$dest/libfpv-live/libfpv-$(date +%s).dylib"
    echo "libfpv: new build (the running game switches to it with the core)"
  fi
  put "$build/portal2-fpv.so" "$dest/portal2-fpv.so"
  cp "$here/addons/portal2-fpv.vdf" "$game/portal2/addons/portal2-fpv.vdf"
  if [ -d "$here/content" ]; then cp -R "$here/content/." "$game/portal2/"; fi
fi
put "$build/core.dylib" "$dest/core.dylib"
echo "deployed ($mode, kamikaze=$kamikaze) to $dest"
