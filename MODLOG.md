# Portal 2 FPV — MODLOG

Development journal. Historical: when it disagrees with libfpv's
docs/INTEGRATION.md or libfpv.h, those win.

## Environment (recon 2026-11-10)

- Portal 2 PatchVersion 2.0.0.1 (steam.inf), 32-bit (`portal2.exe`, `engine.dll`
  PE32). D3D9 through `shaderapidx9` (d3d9.dll); `-vulkan` uses `shaderapivk`
  over `dxvk_d3d9.dll`. A native Linux build is there too (not targeted yet).
- Interfaces: VEngineClient015, VEngineServer022, EngineTraceServer004,
  VEngineCvar007, VClient016, VClientEntityList003, VSERVERTOOLS001,
  ServerGameDLL005, PlayerInfoManager002, VFileSystem017.
- `engine.dll` loads server plugins from `addons/*.vdf` at startup; callbacks
  are version 3 (ClientFullyConnect after LevelShutdown).
- Portal 2 Authoring Tools installed: studiomdl.exe, vtex.exe, hlmv.exe, hammer.
- Default binds that matter: F7 = `load quick` (the mod takes it while
  loaded), E = +use, F = +mouse_menu (coop), C = voice, R = swap_ss_input,
  T = say, Q = taunt menu. F8, Y, G, 1, 2 are free.
- Workshop: maps only (BSP + packed content + VScript). No native code, so
  the mod can't be a Workshop item; it ships as a plugin zip.

## Decisions (agreed with the user 2026-11-10)

- Plugin zip, no Workshop map. Windows first. Single player only.
- Radio: fire portals (limited to what Chell's gun has), use, grab, fly
  through portals keeping momentum. All puzzle elements react.
- Pilot view (F): Chell walks while the drone keeps doing things (two players).
- Enemies aim at the drone: an npc_bullseye follows it (no stand-in needed;
  chambers are small and nothing streams).
- A real .mdl for the drone (generated, compiled with studiomdl).
- libfpv extended for this (uncommitted in ~/Projects/libfpv): game controls
  on radio switches, linked portals, pushes, cargo.

## libfpv changes made for this mod

- `fpv_controls_set` / `fpv_control_poll`: the game's own actions on radio
  switches/buttons (per-radio, by name, in radio_mappings.json), keys
  (settings.json) and gamepad buttons; the wizard sets them up next to the
  flight modes ("Set up game controls"), F8 > Controls lists them.
- `fpv_portals_set`: linked openings; the sim passes the drone through at
  1 kHz (pose, velocity, spin turned), drops contacts inside an opening's
  hole, continues raycasts through it, and turns frame.camera through an
  opening the lens has passed. `frame.portal_passes` / `last_portal`.
- `fpv_drone_push`: set or add velocity (faith plates, funnels, gel).
- `fpv_drone_set_cargo`: mass, inertia and a collision sphere.
- Tests: portals, push, cargo, game_controls; wizard: game controls.

## Engine facts used (verify in game)

- ClientMode from IBaseClientDLL::HudProcessInput (vtable 10): call at +4 to
  GetClientMode, g_pClientMode at GetClientMode+19 (SAR). OverrideView 18,
  CreateMove 24.
- Server entity list: IServerTools vtable 1 (GetIServerEntity) + 61 = CEntInfo[]
  (SAR). 0x2000 entries, handle = index (16 bits) | serial << 16.
- CBaseEntity: GetServerClass 9, GetDataDescMap 11, GetRefEHandle 2.
  C_BaseEntity: GetDataDescMap 15, GetPredDescMap 17. GetClientClass through
  IClientNetworkable (entity + 8), slot 2; slot 18 of the entity is not it.
- D3D9: the game's device is D3D9Ex (shaderapidx9+da5d8 holds it). d3d9.dll
  gives each device its own heap copy of the vtable, so patching a dummy
  device's vtable does nothing: the loader finds the game's device (the
  object in shaderapidx9's data whose slot 17 is d3d9's Present) and patches
  its vtable (Present 17, Reset 16, PresentEx 121, ResetEx 132).
- npc_bullseye is solid: TraceFirePortal from the drone started inside it
  (result 9, PASSTHROUGH_SURFACE). It is made SOLID_NONE around the call.
- The console's `jpeg` command screenshots the game without the mod's hook.
- The cursor: Source re-centres it every frame through user32 SetCursorPos
  imported by inputsystem.dll (and vgui2.dll), even with cl_mouseenable 0.
  While the overlay is open those import slots point to a no-op.
- Console commands run from inside the server's GameFrame (`map`, `load`)
  shut the server down from inside its frame ("pack entities with invalid
  local client state") and drop the game to the menu. The bridge runs from
  a SetTimer timer on the main thread (between frames, also at the menu).
- Field names (server.dll): env_portal_laser m_vStartPoint / m_vEndPoint /
  m_bLaserOn; projected_tractor_beam_entity m_vecStartPoint /
  m_vecEndPoint, m_hOwnerEntity; prop_tractor_beam m_bEnabled,
  m_flLinearForce (negative = reversed); trigger_catapult
  m_flPhysicsVelocity / m_flPlayerVelocity, m_hLaunchTarget,
  m_vecLaunchAngles; trigger_hurt m_flDamage (goo: hundreds).
- A drone spawned with its centre on a floor's surface fell through it (the
  rays start in the solid): spawn 4 units up.
- Paint (gel) has no trace flag; it needs IVEngineServer's
  SphereTracePaintSurface (vtable slot not known yet).
- IServerTools: SetKeyValue(char) 13, CreateEntityByName 14, DispatchSpawn 15.
- PrecacheModel is IEngineTool (VENGINETOOL003) 61, not IVEngineServer
  (calling 61 there crashed in engine.dll). IVEngineClient: GetScreenSize 5,
  GetViewAngles 18, SetViewAngles 19, IsPaused 86, ExecuteClientCmd 104.
- ICvar::FindCommandBase 13; ConCommandBase::m_nFlags at +20.
- CWeaponPortalgun::TraceFirePortal and FindPortal by SAR's patterns (both
  match once in this server.dll). Placement result <= 2 is a success.
- VScript (server): no EntIndexToHScript; FindByClassnameNearest, FindByName,
  SetOrigin/SetAngles/SetVelocity, EntFireByHandle. The console splits on ';',
  so the mod writes scripts/vscripts/p2fpv.nut with one helper per action.

## Log

- 2026-11-10: libfpv extended and tested (Linux 41 tests / 794 checks,
  wizard 75 + 95; win32 built). Plugin written: loader
  (hot reload, D3D9 hooks, cmd.txt bridge), core (sdk, drone, world,
  gameplay, render, audio, devtools). Model generated and compiled.
  Nothing tested in game yet.

- 2026-11-10, first session in game (sp_a2_dual_lasers): fixed the
  PrecacheModel and GetClientClass crashes, added crash.txt (a vectored
  handler: module+offset and the stack's return addresses). Verified: launch,
  FPV / chase / LOS cameras, HUD hidden, Chell in third person, collision
  (flew into the girders), the model and its colours, firing portals from the
  drone (bumped next to the other), flying through a portal pair with the
  velocity turned and the camera through. The D3D9 hook rewritten (never
  fired before): needs the restart to verify.

- 2026-11-10, second session in game: D3D9 hook verified (OSD, fisheye,
  overlay drawn). Fixed: the overlay's mouse (cursor re-centred), the laser
  and funnel field names, the faith plate aim (libfpv's real gravity, held
  on the arc against drag), goo (a lethal trigger_hurt or falling out of the
  world respawns the drone; a respawn loop on the stale frame), the drone
  falling through a floor it spawned on, the bridge (`map` from GameFrame
  dropped the game to the menu). Verified by script: lasers, grab/carry/drop
  (a reflector cube keeps redirecting), pedestal button (cube dropper),
  cube on a floor button (the indicator lights), faith plate, goo, turrets
  (deploy and shoot the drone), excursion funnel (carried at its speed),
  fizzler (respawn). Dev: autotest words f8, at=x,y,z[,yaw]; status lists
  lasers and funnels.

## Next — start here

In game, with the hot reloader (tools/deploy.sh core, tools/p2.sh). After a
game restart the bridge works from the main menu: `tools/p2.sh cmd "map X"`.
1. Done: plugin, cameras, OSD/fisheye/overlay, collision, portals (fire and
   pass), use, grab, floor/pedestal buttons, lasers, turrets, faith plates,
   funnels, fizzlers, goo.
2. By hand (the user): the keys F / C / R / Y, the overlay's controls page,
   a radio with the game controls on its switches.
3. Not verified: a cube fizzled while carried (same Dissolve path as
   drop), a turret knocked over by the drone, a reversed funnel.
4. Gel: find IVEngineServer::SphereTracePaintSurface's slot, map paint
   colours to the repulsion / propulsion materials (already declared).
5. The PVS around a far drone (pilot view), the game's sound listener.
6. The "nothing to use" sound (Portal.Button_Locked) once logged a missing
   buttons/button2.wav.
