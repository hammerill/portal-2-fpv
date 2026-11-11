// Local development builds against a kamikaze libfpv (CMake P2FPV_KAMIKAZE,
// tools/deploy.sh): the warhead's explosion in the chamber (env_explosion:
// fireball, sound, damage and a shove for cubes and turrets) and the ending
// the player chose in F8. Never in CI or release builds; empty there.
#pragma once

#include <libfpv.h>

namespace kamikaze {

#if defined(LIBFPV_KAMIKAZE)
void declare();  // libfpv is loaded: the endings Portal supports
// Every frame with a drone (main thread): explosions and the ending's timer.
void frame(const fpv_frame &f, float dt);
void cancel();    // the player respawned, recalled or moved Chell: no ending any more
bool cutscene();  // the ending shows the blast from outside (the LOS camera)
inline bool wrecked(const fpv_frame &f) { return (f.flags & FPV_DRONE_DETONATED) != 0; }
#else
inline void declare() {}
inline void frame(const fpv_frame &, float) {}
inline void cancel() {}
inline bool cutscene() { return false; }
inline bool wrecked(const fpv_frame &) { return false; }
#endif

}  // namespace kamikaze
