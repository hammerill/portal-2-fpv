#!/usr/bin/env bash
# The developer bridge to a running Portal 2 with the mod (see src/devtools.h).
#
#   tools/p2.sh cmd "noclip"        run console commands (one per argument)
#   tools/p2.sh status              the mod's status.txt
#   tools/p2.sh log [n]             the last n lines of portal2-fpv.log (40)
#   tools/p2.sh console [n]         the last n lines of the game console (40)
#   tools/p2.sh shot [out.png]      a screenshot of the next frame (as the player sees it)
#   tools/p2.sh dump class...       every field of an entity class (or player / weapon)
#   tools/p2.sh autotest file|-     a scripted flight (src/devtools.h has the format)
set -euo pipefail
game="${PORTAL2_DIR:-/mnt/d/SteamLibrary/steamapps/common/Portal 2}"
mod="$game/portal2/addons/portal2-fpv"
data="$mod/fpv-data"
mkdir -p "$data"
what="${1:-status}"; shift || true
case "$what" in
  cmd) printf '%s\n' "$@" >> "$data/cmd.txt" ;;
  status) cat "$data/status.txt" ;;
  log) tail -n "${1:-40}" "$mod/portal2-fpv.log" ;;
  console) tail -n "${1:-40}" "$data/console.log" ;;
  shot)
    out="${1:-/tmp/p2shot.png}"
    before="$(ls -t "$data/shots" 2>/dev/null | head -1 || true)"
    : > "$data/shot.txt"
    for _ in $(seq 1 50); do
      sleep 0.2
      latest="$(ls -t "$data/shots" 2>/dev/null | head -1 || true)"
      if [ -n "$latest" ] && [ "$latest" != "$before" ]; then
        sleep 0.3
        ffmpeg -loglevel error -y -i "$data/shots/$latest" -vf "scale='min(1280,iw)':-2" "$out"
        echo "$out"; exit 0
      fi
    done
    echo "no screenshot (is a frame being drawn?)" >&2; exit 1 ;;
  dump)
    printf '%s\n' "$@" > "$data/dump.txt"
    sleep 1
    for c in "$@"; do echo "== $c"; cat "$data/dump-$c.txt" 2>/dev/null | head -400; done ;;
  autotest)
    if [ "${1:-}" = - ]; then cat > "$data/autotest.txt"; else cp "$1" "$data/autotest.txt"; fi ;;
  *) echo "unknown: $what" >&2; exit 2 ;;
esac
