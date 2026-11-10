// What the drone collides with and flies through: the engine's own traces
// (libfpv's raycast callback), surfaces that break props (goo, water), and
// Chell's open portal pairs handed to libfpv every frame.
#pragma once

#include <cstdint>

#include "sdk.h"

namespace world {

// Materials of the hits (fpv_material_set slots).
enum Material : uint8_t { kDefault = 0, kLiquid = 1, kGlass = 2, kGrate = 3, kRepulsionGel = 4, kPropulsionGel = 5 };

void install();             // libfpv is loaded: raycast callback and materials
void before_tick(float dt); // main thread, before fpv_drone_tick: the portals
void reset();               // the level ended

// The near plane for the FPV camera at `pos` looking along `fwd`: 0.8 x the
// closest geometry in the frustum, in units (docs/INTEGRATION.md#coordinates-and-the-camera).
float near_plane(const sdk::Vector &pos, const sdk::Vector &fwd, const sdk::Vector &up, float aspect, float fov_y_deg);

// object_id of an impact <-> the entity it hit.
uint32_t object_id(void *ent);
void *object_entity(uint32_t id);

// Open, linked portal pairs this frame (for the gameplay's own checks).
struct PortalPair {
    void *a, *b;
};
int portal_pairs(PortalPair *out, int max);

}  // namespace world
