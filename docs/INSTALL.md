# Install Portal 2 FPV

Fly an FPV drone through Aperture's test chambers with your own radio,
gamepad or keyboard, and solve them with it. This takes about two minutes.

**[Download portal-2-fpv.zip](https://github.com/hammerill/portal-2-fpv/releases/latest/download/portal-2-fpv.zip)**
(Windows) · **[portal-2-fpv-macos.zip](https://github.com/hammerill/portal-2-fpv/releases/latest/download/portal-2-fpv-macos.zip)**
(Mac with Apple silicon, [see below](#on-a-mac-apple-silicon))

## 1. Have the right game

You need **Portal 2 on Steam for Windows**, kept up to date by Steam
([store page](https://store.steampowered.com/app/620/)). No other mods or
tools are needed. Single player only.

Find the game folder: in Steam, right-click Portal 2 > Manage > Browse local
files. That opens the **`Portal 2`** folder (the one with `portal2.exe` and a
`portal2` folder in it). Close the game before the next step.

## 2. Install Portal 2 FPV

1. Download [**portal-2-fpv.zip**](https://github.com/hammerill/portal-2-fpv/releases/latest/download/portal-2-fpv.zip).
2. Extract everything into the `Portal 2` folder. If Windows asks, let it
   merge the `portal2` folder with the existing one.

Your `Portal 2` folder now has, among the game's own files:

```
Portal 2/
├── portal2.exe
└── portal2/
    ├── addons/
    │   ├── portal2-fpv.vdf            (this mod)
    │   └── portal2-fpv/               (this mod)
    │       ├── portal2-fpv.dll
    │       ├── core.dll
    │       ├── libfpv.dll
    │       └── libfpv.LICENSE.txt
    ├── models/fpv/                    (this mod: the drone)
    └── materials/models/fpv/          (this mod: the drone)
```

Portal 2 loads the mod by itself at startup; there is nothing to enable.

## On a Mac (Apple silicon)

The Mac version runs in **Portal 2 Natif**, the native Mac app that
[source-recomp](https://github.com/kanapitsas/source-recomp) builds from your
own Steam copy of Portal 2. The mod needs a source-recomp that loads native
plugins (a Mac library in place of a Linux one in `addons/`); an older build
starts the game without the mod.

1. Install Portal 2 Natif with source-recomp and check that the game runs.
2. Find the Steam game folder: in Steam, right-click Portal 2 > Manage >
   Browse local files. On a Mac it is usually
   `~/Library/Application Support/Steam/steamapps/common/Portal 2`. Close
   the game.
3. Download [**portal-2-fpv-macos.zip**](https://github.com/hammerill/portal-2-fpv/releases/latest/download/portal-2-fpv-macos.zip)
   and double-click it to unzip. Copy what is inside its `portal2` folder
   into the game's `portal2` folder (`addons`, `models`, `materials`;
   merge folders, don't replace them).

The files are the same as on Windows, with Mac libraries:
`addons/portal2-fpv/portal2-fpv.so` (a Mac library: the game's Linux engine
asks for that name), `core.dylib` and `libfpv.dylib`. Start **Portal 2 Natif**
and fly as below. Settings live in `~/Library/Application Support/libfpv`; the
log is `addons/portal2-fpv/portal2-fpv.log` in the game folder, and the
game's own log is `~/Library/Logs/Portal2Natif.log`. Updating and
uninstalling are as on Windows, with these files.

## 3. Fly

Start Portal 2, start or load a single player chamber and press **F7**. The
drone launches in front of Chell.

| Key | Action |
|---|---|
| F7 | Launch the drone / end the flight / back to the drone after F |
| F8 | Settings: radio setup, game controls, rates, OSD, camera |
| F | Back to Chell; the drone keeps flying |
| C | Cycle cameras: FPV, chase, line of sight |
| R | Respawn the drone in front of Chell, repaired |
| Y | Move Chell to the drone |
| Mouse 1 / 1 | Fire the blue portal from the drone |
| Mouse 2 / 2 | Fire the orange portal |
| E | Use: press the button the drone looks at |
| G | Grab / drop a cube under the drone |

The drone fires only the portal colours Chell's gun has at that point of the
game.

- **Radio:** plug it in (USB joystick mode), press F8 and follow the radio
  setup wizard once. Your setup is remembered. Then F8 > Radio > "Set up game
  controls" puts fire, use and grab on your radio's switches and buttons.
- **Keyboard:** Enter arms, W/S is throttle, A/D is yaw, and the arrow keys
  are pitch and roll. The keys in the table can be changed in F8 > Controls.
- **Gamepad:** works too. Left / right trigger fire portals, X (Square) uses,
  left shoulder grabs.

Portal 2 normally loads the quicksave with F7. While the mod is loaded F7 is
the drone's; the game gets it back when the mod is removed.

## Tune it in Betaflight Configurator

The drone runs real Betaflight, so you can set it up like a real quad, with
PIDs, filters, rates and modes, in
[Betaflight Configurator](https://app.betaflight.com) (the web app or the
desktop app):

1. In the game, launch the drone with F7.
2. In Configurator, open Options and turn on **manual connection mode**.
3. Pick **Manual Selection**, enter `ws://localhost:5761` as the port and
   press Connect.

F8 > Configurator shows the same address. While Configurator is connected,
Betaflight won't arm, the same as a real quad, so disconnect it before you
fly. Keep the motor protocol on PWM and ARM on AUX1.

## Updating

Close the game, download the newest `portal-2-fpv.zip` and extract it over
the old files. Your settings are kept.

## Uninstalling

In the `Portal 2/portal2` folder, delete:

- `addons/portal2-fpv.vdf`
- the `addons/portal2-fpv` folder
- the `models/fpv` folder
- the `materials/models/fpv` folder
- `scripts/vscripts/p2fpv.nut` (the mod writes it when it runs)

Your radio setup, rates and Betaflight tune are in `%APPDATA%\libfpv`, shared
with other libfpv games. Delete that folder too if you don't fly any of them.

## Something wrong?

- **F7 does nothing / still loads the quicksave:** check that
  `portal2-fpv.vdf` is in `Portal 2/portal2/addons/` and the
  `portal2-fpv` folder next to it, not one folder deeper (for example
  `Portal 2/portal-2-fpv/portal2/...`), then restart the game.
- **The drone is an error model or invisible:** the `models` and
  `materials` folders didn't end up inside `Portal 2/portal2/`; extract
  the ZIP again into the `Portal 2` folder.
- **Anything else:** look in
  `Portal 2/portal2/addons/portal2-fpv/portal2-fpv.log` (and
  `fpv-data/crash.txt` next to it, if the game crashed), then
  [open an issue](https://github.com/hammerill/portal-2-fpv/issues) and
  attach those files.
