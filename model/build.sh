#!/usr/bin/env bash
# Builds the drone model: generates the sources (build_model.py), compiles
# them with Portal 2's studiomdl (Portal 2 Authoring Tools), and keeps the
# result in content/ (tools/deploy.sh copies content/ into portal2/).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
game="${PORTAL2_DIR:-/mnt/d/SteamLibrary/steamapps/common/Portal 2}"
python3 -I "$here/build_model.py"
mkdir -p "$here/../content/models/fpv"
# studiomdl can't read from WSL's file system: compile a copy in Windows' temp
# folder. It writes into the game's models/ folder.
wintmp="$(wslpath -u "$(cmd.exe /c 'echo %TEMP%' 2>/dev/null | tr -d '\r')")/portal2-fpv-model"
rm -rf "$wintmp" && mkdir -p "$wintmp" && cp "$here/build/"* "$wintmp/"
"$game/bin/studiomdl.exe" -nop4 -game "$(wslpath -w "$game/portal2")" "$(wslpath -w "$wintmp/quad5.qc")" \
  | tr -d '\r' | grep -v -E "^\s*$" | tail -25
cp "$game/portal2/models/fpv/"quad5.* "$here/../content/models/fpv/"
ls -la "$here/../content/models/fpv"
