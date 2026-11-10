# Portal 2 FPV

Fly a real FPV quadcopter through Aperture's test chambers with your own radio,
and solve them with it: fire portals from the drone, press buttons, carry
cubes to buttons, fly through your portals with all your momentum, ride faith
plates and excursion funnels, and dodge turrets, lasers, fizzlers and goo.

The drone is [libfpv](https://github.com/hammerill/libfpv): real Betaflight
firmware in the loop, a Betaflight OSD, the fisheye of an FPV camera, motor
sound, and an F8 overlay with a radio setup wizard.

> **Status:** in development. Tested in game (Windows): flight, cameras,
> portals, use, grab, buttons, lasers, turrets, faith plates, funnels,
> fizzlers and goo. Gel isn't in yet.

## Features

- **Fly it like a real quad.** Radio (USB joystick mode), gamepad or keyboard.
  Betaflight rates, modes and tune; Betaflight Configurator connects too.
- **The drone is the portal gun.** Blue and orange portals fire along the
  drone's camera with the gun's own rules (portalable surfaces, fizzlers, room
  to fit), but only the colours Chell's gun has at that point of the game.
- **Through the portals.** The drone keeps its speed and spin through a
  portal pair, turned with it: dive into a floor portal, fly out of the wall.
- **Hands on the sticks.** Fire, use and grab go on your radio's switches
  and buttons (F8 > Radio > Set up game controls), next to the flight modes.
- **Use and grab.** Press pedestal buttons from the air; pick up a cube (or a
  turret) under the drone, carry it with its weight and drop it on a button.
- **The chamber fights back.** Turrets shoot the drone, lasers burn its
  props, goo and water drown it, fizzlers fizzle it (and the cube it carries),
  faith plates launch it and excursion funnels carry it.
- **Two of you.** Press F to walk as Chell while the drone hovers: a friend on
  the radio holds a button, fires portals or carries a cube, while you walk.

## Controls

| Key | Action |
|---|---|
| F7 | Launch the drone / end the flight / back to the drone after F |
| F | Back to Chell, the drone left flying |
| F8 | Settings overlay (radio setup, game controls, rates, OSD, camera) |
| C | Cycle cameras: FPV, chase, line of sight |
| R | Respawn the drone in front of Chell, repaired |
| Y | Move Chell here (also a button in F8 > Fly) |
| Mouse 1 / 1 | Fire blue portal (in the drone's views) |
| Mouse 2 / 2 | Fire orange portal |
| E | Use: press the button the drone looks at |
| G | Grab / drop a cube under the drone |

Fire, use and grab can go on the radio (F8 > Radio > "Set up game controls");
the keys can be changed in F8 > Controls. Gamepad: left / right trigger fire,
X / Square uses, left shoulder grabs.

Without a radio the keyboard flies: Enter arms, W/S throttle, A/D yaw, arrows
pitch and roll.

Portal 2 loads the quicksave with F7; while the mod is loaded F7 is the
drone's, and the binding is given back when the mod unloads.

## Installing

1. Copy the release's `portal2` folder into
   `Steam/steamapps/common/Portal 2/` (it adds `portal2/addons/portal2-fpv*`,
   `portal2/models/fpv` and `portal2/materials/models/fpv`).
2. Start Portal 2, load a chamber, press F7.

Portal 2 loads the plugin by itself from `portal2/addons/portal2-fpv.vdf`.
Single player only. To uninstall, delete those files.

## Building

Cross-compiled from Linux (or WSL) with MinGW-w64, as 32-bit Windows DLLs:

```sh
# libfpv for 32-bit Windows (once)
cmake -S ~/Projects/libfpv -B ~/Projects/libfpv/build/win32 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-i686.cmake
# the mod, copied into the game
tools/deploy.sh          # with the game closed
tools/deploy.sh core     # while it runs: the core reloads in place
# the drone's model (needs the Portal 2 Authoring Tools)
model/build.sh
```

## How it works

- **Loading.** `portal2-fpv.dll` is a server plugin (`IServerPluginCallbacks`).
  It stays loaded and hot-reloads `core.dll`, the mod itself, whenever a new
  build appears, so it can be tested without restarting the game.
- **Camera.** `ClientModeShared::OverrideView` puts the engine's view at the
  drone's camera, with full roll and the widened FOV for the fisheye. The
  engine renders everything from there, portals and their recursion included.
- **Chell.** `ClientModeShared::CreateMove` holds her still while you fly; she
  is drawn in third person so the drone sees its pilot.
- **Collision.** The engine's own traces (`IEngineTrace::TraceRay`) answer
  libfpv's rays. libfpv knows the open portal pairs, so the wall around a
  portal doesn't stop the drone, and it takes the drone through.
- **Portals from the drone.** The portal gun's `TraceFirePortal` decides where
  a portal goes, from the drone's camera; `portal_place` puts it there.
- **Turrets** target an `npc_bullseye` that follows the drone; the damage it
  takes breaks props. Lasers, fizzlers, hurt triggers, faith plates and
  funnels are read from the entities each server tick.
- **Drawing.** A D3D9 `Present` hook (d3d9 and DXVK/`-vulkan`) draws the
  fisheye, the OSD and the overlay.
- **Model.** An original 5" quad (`model/build_model.py`), compiled with
  studiomdl; the client copy is moved to the drone every frame.

`MODLOG.md` is the development journal.

## Testing without hands

`tools/p2.sh` talks to a running game: console commands, the mod's status,
screenshots, entity field dumps and scripted flights (see `src/devtools.h`).

## Credits

- [libfpv](https://github.com/hammerill/libfpv): the drone, Betaflight (through
  SimITL), OSD, overlay and sound.
- [SourceAutoRecord](https://github.com/p2sr/SourceAutoRecord) (MIT): the
  vtable indices, struct layouts and code patterns for the current Portal 2.
- The drone model is generated by this mod's own script (AI-written code, no
  outside assets). Sounds are the game's own.
- Written with Claude (Anthropic).

## License

GPL-3.0-or-later, the same as libfpv (which embeds Betaflight). See
[LICENSE](LICENSE).
