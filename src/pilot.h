// Chell, the pilot.
//
// - Hidden while a drone exists: she would only be seen in the way of the
//   drone's views, and through portals.
// - The stand-in (libfpv's FPV_PILOT_STAND_IN, on by default, F8 > Fly >
//   Pilot): while the drone is farther than the stand-in distance from where
//   she stands, she goes along with it, unseen and through walls (noclip),
//   and the game plays against the drone as if it were her: turrets target
//   and shoot at its exact position, doors open for it, the PVS and
//   areaportals are its own, and the damage she takes breaks its props
//   instead. She is back at her spot once the drone returns within the
//   return distance, in the pilot view (F), at a respawn, and when the
//   flight ends.
#pragma once

#include <libfpv.h>

#include "sdk.h"

namespace pilot {

// libfpv is loaded: what the game supports (the stand-in, its distances).
void declare();

void begin();  // a flight starts: Chell hidden
void end();    // the flight ends: Chell back at her spot, shown
// Back at the spot now (F, a respawn); the stand-in takes over again later.
void home();
// Y: she stays where the drone is (the caller moves her there).
void release();

// Every rendered frame with a drone (main thread): starts or ends the stand-in.
void frame(const fpv_frame &f, bool drone_view);
// The server frame: keeps her with the drone.
void server_tick();

bool standing_in();
// Where the pilot stands (feet): the spot she left, or Chell herself.
sdk::Vector spot();

}  // namespace pilot
