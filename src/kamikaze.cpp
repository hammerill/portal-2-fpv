#include "kamikaze.h"

#if defined(LIBFPV_KAMIKAZE)

#include <cstdio>

#include "drone.h"
#include "fpvapi.h"
#include "log.h"
#include "sdk.h"

namespace kamikaze {
namespace {

using fpvapi::fpv;
using sdk::Vector;

// A 5" quad's warhead in Portal's units: kills a turret close by, tips the
// ones further out, shoves cubes, and hurts Chell if she stands next to it.
constexpr const char *kMagnitude = "80";
constexpr const char *kRadius = "220";

bool g_ending = false;
float g_left = 0;
bool g_cutscene = false;
uint32_t g_then = FPV_KAMIKAZE_THEN_RESPAWN;
int g_booms = 0;

void explode(const Vector &at) {
    char name[32], origin[64];
    std::snprintf(name, sizeof name, "p2fpv_boom%d", g_booms++);
    std::snprintf(origin, sizeof origin, "%.1f %.1f %.1f", at.x, at.y, at.z);
    void *e = sdk::create_entity("env_explosion");
    if (!e) {
        fpvlog::write("kamikaze: no env_explosion");
        return;
    }
    sdk::set_keyvalue(e, "targetname", name);
    sdk::set_keyvalue(e, "origin", origin);
    sdk::set_keyvalue(e, "iMagnitude", kMagnitude);
    sdk::set_keyvalue(e, "iRadiusOverride", kRadius);
    sdk::dispatch_spawn(e);
    sdk::uncheat("ent_fire");
    sdk::execf("ent_fire %s Explode", name);
    sdk::execf("ent_fire %s Kill \"\" 3", name);
}

void act() {
    g_ending = false;
    g_cutscene = false;
    if (g_then == FPV_KAMIKAZE_THEN_RESPAWN) drone::respawn();
    else if (g_then == FPV_KAMIKAZE_THEN_RECALL) drone::stop();
}

}  // namespace

void declare() {
    fpv.fpv_kamikaze_set_ending_support(
        (1u << FPV_KAMIKAZE_VIEW_SIGNAL_LOST) | (1u << FPV_KAMIKAZE_VIEW_CUTSCENE),
        (1u << FPV_KAMIKAZE_THEN_RESPAWN) | (1u << FPV_KAMIKAZE_THEN_RECALL) | (1u << FPV_KAMIKAZE_THEN_NOTHING),
        FPV_KAMIKAZE_VIEW_SIGNAL_LOST, FPV_KAMIKAZE_THEN_RESPAWN);
}

void frame(const fpv_frame &, float dt) {
    fpv_detonation d{};
    d.struct_size = sizeof d;
    while (fpv.fpv_kamikaze_poll_detonation(&d)) {
        Vector at(d.position);
        fpvlog::write("kamikaze: detonation (cause %d) at %.0f %.0f %.0f", int(d.cause), at.x, at.y, at.z);
        explode(at);
        fpv_kamikaze_config c{};
        c.struct_size = sizeof c;
        fpv.fpv_kamikaze_get_config(&c);
        g_ending = true;
        g_left = c.ending_duration;
        g_cutscene = c.ending_view == FPV_KAMIKAZE_VIEW_CUTSCENE;
        g_then = c.ending_then;
        if (g_left <= 0 && g_then != FPV_KAMIKAZE_THEN_NOTHING) {
            act();
            return;
        }
    }
    if (!g_ending || sdk::is_paused()) return;
    g_left -= dt;
    if (g_left <= 0 && g_then != FPV_KAMIKAZE_THEN_NOTHING) act();
}

void cancel() {
    g_ending = false;
    g_cutscene = false;
}

bool cutscene() { return g_ending && g_cutscene; }

}  // namespace kamikaze

#endif
