#include "pilot.h"

#include <cstdio>

#include "fpvapi.h"
#include "gameplay.h"
#include "log.h"
#include "platform.h"

namespace pilot {
namespace {

using fpvapi::fpv;
using fpvapi::kUnitsPerMeter;
using sdk::Vector;

// Portal's chambers are small, and a door opens for Chell a few metres
// before she reaches it: she follows the drone soon after it leaves her.
constexpr float kStandInDistance = 2.0f * kUnitsPerMeter;
constexpr float kReturnDistance = 1.0f * kUnitsPerMeter;
// Her feet above the drone's centre while she stands in: her hull spans the
// air above it, in a door's trigger whenever the drone is, but clear of a
// floor button under a drone that lands on it (that's the cube's job). Her
// eye is on the drone's centre (the view offset points down to it): the
// game aims at a player between 70 % and 100 % of the way from the feet to
// the eye (CBasePlayer::BodyTarget), so every shot goes within 4 units of
// the drone, in all three axes; the PVS is the drone's too.
constexpr float kFeetAboveDrone = 12;
constexpr float kEyeHeight = 36;  // her hull's middle above the feet (for distances)

// Her health while she stands in: whatever the game takes from it breaks the
// drone's props instead, and it is topped up again (she never dies of it).
constexpr int kStandInHealth = 100000;
constexpr uint8_t kMoveWalk = 2, kMoveNoclip = 8;

bool g_flying = false;
bool g_standing_in = false;
Vector g_spot;  // her feet when the stand-in began (relative to g_ground's origin if she stood on something)
uint32_t g_ground = 0xFFFFFFFFu;  // what she stood on, when it moves (the arrival elevator)
Vector g_drone;
bool g_have_drone = false;
// Her eye goes to the drone: the server sends the client what is
// visible from the player's eye (the PVS, areaportals), and from 76 units
// above the drone it was often inside a ceiling, so things near the drone
// flickered out for a moment. Her hull stays above the drone (floor buttons).
Vector g_view_offset{0, 0, 64};
int g_health = 100;  // hers before the stand-in
// Sent home: the move lands a frame or so later; until then she is there
// already, as far as the spot and the next stand-in are concerned.
bool g_going_home = false;
Vector g_home;
uint32_t g_home_since = 0;

Vector feet() { return sdk::abs_origin(sdk::player()); }

// The player by a plain input (no script needed); her gun through a helper.
void draw(bool on) {
    sdk::uncheat("ent_fire");
    sdk::execf("ent_fire !player %s", on ? "EnableDraw" : "DisableDraw");
    sdk::execf("script p2fpv_draw_player(%d)", on ? 1 : 0);
}

// The game's view vectors (CViewVectors, in server.dll / server.so): the player's movement
// resets her view offset to their m_vView every tick, so writing the offset
// alone lasted until the next player command. While she stands in, m_vView
// is the drone's centre seen from her feet (straight down kFeetAboveDrone).
guest::addr view_vectors() {
    static guest::addr vv = 0;
    static bool searched = false;
    if (searched) return vv;
    searched = true;
    // m_vView (0 0 64), m_vHullMin (-16 -16 0), m_vHullMax (16 16 72)
    vv = sdk::scan("server",
                   "00 00 00 00 00 00 00 00 00 00 80 42 00 00 80 C1 00 00 80 C1 00 00 00 00 00 00 80 41 00 00 80 41 00 00 90 42");
    fpvlog::write("pilot: view vectors at %08x", vv);
    return vv;
}

void set_view_height(float z) {
    guest::addr vv = view_vectors();
    const float v[3] = {0, 0, z};
    if (vv) guest::patch(vv, v, sizeof v);
}

// She becomes the drone for the game: targeted and hurt there (the hurt goes
// to the drone), moving through walls with it.
void set_standing_in(bool on) {
    void *p = sdk::player();
    if (!p) return;
    if (int *hp = sdk::field<int>(p, "m_iHealth")) {
        if (on) g_health = *hp, *hp = kStandInHealth;
        else *hp = g_health;
    }
    // through walls: the noclip command needs sv_cheats (its handler checks), the move type doesn't
    if (uint8_t *mt = sdk::field<uint8_t>(p, "m_MoveType")) *mt = on ? kMoveNoclip : kMoveWalk;
    if (Vector *vo = sdk::field<Vector>(p, "m_vecViewOffset")) {
        if (on) g_view_offset = *vo;
        else *vo = g_view_offset;
    }
    set_view_height(on ? -kFeetAboveDrone : 64);
}

void start_standing_in() {
    g_spot = feet();
    // on something that moves (an elevator, a platform): the spot moves with it
    g_ground = 0xFFFFFFFFu;
    uint32_t h = sdk::get<uint32_t>(sdk::player(), "m_hGroundEntity", 0xFFFFFFFFu);
    if (void *g = sdk::entity_from_handle(h); g && sdk::entity_index(g) != 0) {
        g_ground = h;
        g_spot = g_spot - sdk::abs_origin(g);
        fpvlog::write("pilot: Chell stands on %s '%s'", sdk::classname(g), sdk::targetname(g));
    }
    g_standing_in = true;
    set_standing_in(true);
    Vector at = spot();
    fpvlog::write("pilot: stand-in starts, Chell's spot %.0f %.0f %.0f", at.x, at.y, at.z);
}

Vector spot_of_stand_in() {
    void *g = sdk::entity_from_handle(g_ground);
    return g ? sdk::abs_origin(g) + g_spot : g_spot;
}

// Still on her way home (see g_going_home).
bool going_home() {
    if (g_going_home && ((feet() - g_home).length() < 8 || platform::ticks_ms() - g_home_since > 500)) g_going_home = false;
    return g_going_home;
}

void stop_standing_in(bool go_home) {
    if (!g_standing_in) return;
    g_standing_in = false;
    if (go_home) {
        gameplay::install_script();  // a save loaded meanwhile reset the helpers
        Vector at = spot_of_stand_in();
        sdk::execf("script p2fpv_move_player(%.2f, %.2f, %.2f)", at.x, at.y, at.z + 1);
        g_going_home = true;
        g_home = at;
        g_home_since = platform::ticks_ms();
    }
    set_standing_in(false);
    fpvlog::write("pilot: stand-in ends%s", go_home ? ", Chell back at her spot" : "");
}

}  // namespace

void declare() {
    fpv.fpv_pilot_set_support(FPV_PILOT_STAND_IN, kStandInDistance, kReturnDistance);
}

void begin() {
    g_flying = true;
    g_have_drone = false;
    draw(false);
}

void end() {
    if (!g_flying) return;
    gameplay::install_script();
    stop_standing_in(true);
    g_flying = false;
    draw(true);
}

void home() { stop_standing_in(true); }

void release() { stop_standing_in(false); }

void frame(const fpv_frame &f, bool drone_view) {
    if (!g_flying) return;
    g_drone = Vector(f.position);
    g_have_drone = true;
    fpv_pilot_config pc{};
    pc.struct_size = sizeof pc;
    bool on = fpv.fpv_pilot_get_config(&pc) == FPV_OK && (pc.features & FPV_PILOT_STAND_IN);
    Vector at = spot() + Vector{0, 0, kEyeHeight};
    float d = (g_drone - at).length();
    if (!g_standing_in) {
        if (on && drone_view && d > pc.stand_in_distance && !going_home()) start_standing_in();
    } else if (!on || !drone_view || d < pc.stand_in_return_distance) {
        stop_standing_in(true);
    }
}

void server_tick() {
    if (!g_standing_in || !g_have_drone) return;
    Vector at = g_drone + Vector{0, 0, kFeetAboveDrone};
    sdk::execf("script p2fpv_move_player(%.2f, %.2f, %.2f)", at.x, at.y, at.z);
    void *p = sdk::player();
    if (Vector *vo = sdk::field<Vector>(p, "m_vecViewOffset")) *vo = g_drone - at;
    if (int *hp = sdk::field<int>(p, "m_iHealth"); hp && *hp < kStandInHealth) {
        int lost = kStandInHealth - *hp;
        *hp = kStandInHealth;
        gameplay::hit(lost);
        fpvlog::write("pilot: the stand-in took %d damage: the drone's", lost);
    }
}

bool standing_in() { return g_standing_in; }

Vector spot() {
    if (g_standing_in) return spot_of_stand_in();
    return going_home() ? g_home : feet();
}

}  // namespace pilot
