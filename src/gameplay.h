// What the drone does in the test chambers: its own controls (fire portals,
// use, grab), the entities that stand for it (its model, the target turrets
// shoot at), and the chamber's reactions (fizzlers, goo, lasers, turrets,
// faith plates, excursion funnels, carried cubes).
#pragma once

#include <libfpv.h>

#include "sdk.h"

namespace gameplay {

// Writes and runs the VScript helpers (p2fpv_*). Loading a save resets the
// script VM, so call it before relying on them.
void install_script();

// libfpv is loaded: the game controls for the radio, keys and gamepad.
void declare_controls();

void on_launch(const float pos[3]);
void on_respawn();
// Dev: the drone moved without a respawn (the cargo stays).
void on_teleport();
void on_stop();
void level_shutdown();
void server_activate();  // the map is loading: precache

// Entities a trace from the drone must pass through (its own, the cargo).
void add_own_entities(sdk::TraceFilter &filter);

// Main thread, every rendered frame with a drone. `mouse`: the mouse buttons
// fire portals (a drone view, the overlay closed, the game focused).
void client_tick(const fpv_frame &f, float dt, bool mouse);
// IServerPluginCallbacks::GameFrame (tick rate): the world reacts.
void server_tick();
// Right before the frame renders: the drone's model (and its cargo) placed
// on the client where the drone is now, not where the last tick was.
void pin_visuals(const fpv_frame &f);

// For devtools' autotest: presses the drone's controls as the radio would.
enum class Action { FireBlue, FireOrange, Use, GrabOn, GrabOff };
void press(Action a);

// The game hurt the drone (turret bullets, lasers, its stand-in Chell):
// Source damage points, turned into broken props.
void hit(int damage);

// For devtools' status: what the drone carries, and the last action.
const char *status();

}  // namespace gameplay
