// The flight: launching and ending it, the views (FPV, chase, line of sight,
// back to Chell), the keyboard, the camera handed to the engine and Chell held
// still while the player flies.
#pragma once

#include <libfpv.h>

#include "sdk.h"

namespace drone {

enum class View { Fpv = 0, Chase = 1, Los = 2, Count = 3 };

void load(fpv_log_fn log);  // the core started; libfpv logs through `log` (the loader's)
void unload();  // the core is about to be unloaded
void level_shutdown();

// ClientMode::OverrideView, once per rendered frame on the main thread: the
// whole per-frame update, then the drone's camera into `view`.
void on_view(sdk::CViewSetup *view);
// ClientMode::CreateMove: Chell stands still while the player flies.
void on_create_move(sdk::CUserCmd *cmd);

// For the render thread: the frame to draw the OSD and lens from (false when
// no FPV view is on screen).
bool osd_frame(fpv_frame *out, bool *no_signal);

// The drone exists (flying or left in the world).
fpv_drone *handle();
// The latest frame, main thread (nullptr before the first one).
const fpv_frame *frame();
// The player looks through the drone's cameras (not the pilot view).
bool drone_view();
View view();
// Seconds the drone has existed.
float age();

// The drone back in front of Chell, repaired (R, or after it was fizzled).
void respawn();
// Dev: the drone repaired and put at a point, level, facing yaw (degrees).
void respawn_at(const float pos[3], float yaw);
// Launches a drone in front of Chell (F7, the autotest's "launch").
void start();
// Loads libfpv and declares the game controls (once); false if it failed.
bool load_libfpv();
// The autotest's camera: a drone view, or the pilot's (Chell) when `pilot`.
void set_view(View v, bool pilot);
// Ends the flight (F7, Y).
void stop();

}  // namespace drone
