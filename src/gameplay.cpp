#include "gameplay.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

#include "drone.h"
#include "fpvapi.h"
#include "kamikaze.h"
#include "log.h"
#include "paths.h"
#include "pilot.h"
#include "world.h"

namespace gameplay {
namespace {

using fpvapi::fpv;
using fpvapi::kUnitsPerMeter;
using sdk::QAngle;
using sdk::Vector;

// --- game controls -----------------------------------------------------------------------

enum Control : uint32_t { kFireBlue = 1, kFireOrange = 2, kUse = 3, kGrab = 4 };

// --- the drone's entities ------------------------------------------------------------------

const char *const kModel = "models/fpv/quad5.mdl";
// Until the mod's own model is installed: a small prop of the game's.
const char *const kPlaceholderModel = "models/props_junk/garbage_coffeemug001a_forevergibs.mdl";
const char *const kModelName = "p2fpv_model";
const char *const kEyeName = "p2fpv_eye";  // npc_bullseye: what turrets aim at
const char *const kRelName = "p2fpv_rel";  // ai_relationship: turrets hate it
constexpr float kEyeBehind = 0;            // at the centre: bullets aimed at the drone hit it
constexpr int kEyeHealth = 100000;

uint32_t g_model = 0xFFFFFFFFu, g_eye = 0xFFFFFFFFu, g_rel = 0xFFFFFFFFu;
std::string g_status = "idle";

// --- carried cargo ---------------------------------------------------------------------------

// The cube hangs on a winch line under the drone (libfpv's cargo on a line):
// a body of its own, so the quad keeps its inertia and stays flyable, and
// the pilot feels the weight and the swing.
constexpr float kCargoMassKg = 0.35f;  // a test cube, as far as the motors can tell
constexpr float kGrabReach = 40;       // units between the drone and the cube's surface
constexpr float kLineSlack = 4;        // units of line between the hook and the cube's top

struct Carried {
    uint32_t handle = 0xFFFFFFFFu;
    float yaw = 0;        // the cube's heading relative to the drone's, at the pickup
    Vector up{0, 0, 1};   // the cube's up axis, eased towards the line
    Vector pos, vel;      // libfpv's, this frame
    bool active = false;
} g_cargo;

// --- hazards and reactions -------------------------------------------------------------------

Vector g_prev_pos;  // the drone at the last server tick
bool g_have_prev = false;
std::unordered_map<uint32_t, float> g_cooldown;  // per entity handle, seconds left
float g_server_time = 0;
uint32_t g_passes_seen = 0;
bool g_pinned_valid = false;
Vector g_drone_pos, g_drone_vel;
float g_drone_q[4] = {0, 0, 0, 1};
bool g_fizzle = false;  // fizzled this tick: respawn at Chell next frame
bool g_drowned = false;  // fell in goo (a lethal trigger_hurt) or out of the world: the same
bool g_wrecked = false;  // a kamikaze build's drone after its warhead went off: not drawn, nothing reacts

// A faith plate's flight: Portal's plates fly an exact arc (the player has no
// air control on it); the drone has drag, so it is held on the arc until it
// lands, hits something or goes through a portal.
struct Fling {
    bool active = false;
    Vector v0;
    float t = 0, duration = 0;
};
Fling g_fling;

// --- helpers -----------------------------------------------------------------------------

Vector qrot(const float q[4], const Vector &v) {
    Vector u(q[0], q[1], q[2]);
    float s = q[3];
    return u * (2.0f * u.dot(v)) + v * (s * s - u.dot(u)) + u.cross(v) * (2.0f * s);
}
// The model's nose is along its +Y axis: the entity is yawed 90 degrees right
// of the drone (its +X on the drone's right side) so the nose points forward.
QAngle model_angles(const float q[4]) {
    return sdk::vectors_angles(qrot(q, {0, -1, 0}), qrot(q, {0, 0, 1}));
}

Vector qrot_inv(const float q[4], const Vector &v) {
    float c[4] = {-q[0], -q[1], -q[2], q[3]};
    return qrot(c, v);
}

void sound(const char *name) { sdk::execf("playgamesound %s", name); }

std::string game_dir() {
    std::string d = paths::mod_dir();  // ...\portal2\addons\portal2-fpv
    for (int i = 0; i < 2; i++) d = d.substr(0, d.find_last_of("\\/"));
    return d;
}

bool file_exists(const std::string &rel) {
    return GetFileAttributesA((game_dir() + "\\" + rel).c_str()) != INVALID_FILE_ATTRIBUTES;
}

const char *model_path() { return file_exists(kModel) ? kModel : kPlaceholderModel; }

// The server-side helpers, one call per console command: the console splits
// lines on ';', and Squirrel needs ';' or a new line between statements.
const char *const kScript = R"NUT(// portal2-fpv: helpers the plugin calls with "script p2fpv_...()". Written by the mod.
::p2fpv_place <- function(name, x, y, z, p, yw, r) {
    local e = Entities.FindByName(null, name)
    if (e) { e.SetOrigin(Vector(x, y, z)); e.SetAngles(p, yw, r) }
}
::p2fpv_fire_nearest <- function(cls, x, y, z, input, param) {
    local e = Entities.FindByClassnameNearest(cls, Vector(x, y, z), 4)
    if (e) EntFireByHandle(e, input, param, 0, null, null)
}
::p2fpv_place_nearest <- function(cls, x, y, z, nx, ny, nz, p, yw, r) {
    local e = Entities.FindByClassnameNearest(cls, Vector(x, y, z), 64)
    if (e) { e.SetOrigin(Vector(nx, ny, nz)); e.SetAngles(p, yw, r) }
}
::p2fpv_velocity_nearest <- function(cls, x, y, z, vx, vy, vz, add) {
    local e = Entities.FindByClassnameNearest(cls, Vector(x, y, z), 4)
    if (!e) return
    if (add) e.SetVelocity(e.GetVelocity() + Vector(vx, vy, vz))
    else e.SetVelocity(Vector(vx, vy, vz))
}
::p2fpv_health <- function(name, hp) {
    local e = Entities.FindByName(null, name)
    if (e) e.SetHealth(hp)
}
::p2fpv_sound <- function(name, sound) {
    local e = Entities.FindByName(null, name)
    if (e) e.EmitSound(sound)
}
::p2fpv_draw_player <- function(on) {
    local input = on ? "EnableDraw" : "DisableDraw"
    EntFireByHandle(GetPlayer(), input, "", 0, null, null)
    local g = null
    while (g = Entities.FindByClassname(g, "weapon_portalgun"))
        if (g.GetOwner() == GetPlayer() || g.GetMoveParent() == GetPlayer()) EntFireByHandle(g, input, "", 0, null, null)
}
::p2fpv_move_player <- function(x, y, z) {
    GetPlayer().SetOrigin(Vector(x, y, z))
    GetPlayer().SetVelocity(Vector(0, 0, 0))
}
)NUT";

}  // namespace

void install_script() {
    std::string path = game_dir() + "\\scripts\\vscripts\\p2fpv.nut";
    std::string have;
    if (FILE *f = std::fopen(path.c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) have.append(buf, n);
        std::fclose(f);
    }
    if (have != kScript) {
        CreateDirectoryA((game_dir() + "\\scripts").c_str(), nullptr);
        CreateDirectoryA((game_dir() + "\\scripts\\vscripts").c_str(), nullptr);
        if (FILE *f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(kScript, 1, std::strlen(kScript), f);
            std::fclose(f);
            fpvlog::write("gameplay: wrote %s", path.c_str());
        }
    }
    sdk::exec("script_execute p2fpv");
}

namespace {

void *entity(uint32_t h) { return sdk::entity_from_handle(h); }

// The turrets' target is solid (their bullets must hit it), but the game's
// own traces from the drone (TraceFirePortal) must pass through it: not solid
// for as long as this lives.
class EyeNotSolid {
public:
    EyeNotSolid() {
        void *eye = entity(g_eye);
        if (!eye) return;
        m_type = sdk::field<uint8_t>(eye, "m_Collision.m_nSolidType");
        m_flags = sdk::field<uint16_t>(eye, "m_Collision.m_usSolidFlags");
        if (m_type) m_old_type = *m_type, *m_type = 0;  // SOLID_NONE
        if (m_flags) m_old_flags = *m_flags, *m_flags |= 0x0004;  // FSOLID_NOT_SOLID
    }
    ~EyeNotSolid() {
        if (m_type) *m_type = m_old_type;
        if (m_flags) *m_flags = m_old_flags;
    }

private:
    uint8_t *m_type = nullptr, m_old_type = 0;
    uint16_t *m_flags = nullptr, m_old_flags = 0;
};

void remove_entity(uint32_t &h) {
    if (void *e = entity(h)) sdk::execf("ent_fire %s kill", sdk::targetname(e));
    h = 0xFFFFFFFFu;
}

void spawn_entities(const Vector &at) {
    sdk::uncheat("ent_fire");
    const char *model = model_path();
    sdk::precache_model(model);
    char origin[64];
    std::snprintf(origin, sizeof origin, "%.1f %.1f %.1f", at.x, at.y, at.z);
    if (!entity(g_model)) {
        void *m = sdk::create_entity("prop_dynamic_override");
        if (m) {
            sdk::set_keyvalue(m, "targetname", kModelName);
            sdk::set_keyvalue(m, "model", model);
            sdk::set_keyvalue(m, "solid", "0");
            sdk::set_keyvalue(m, "origin", origin);
            sdk::dispatch_spawn(m);
            g_model = sdk::handle_of(m);
        }
        fpvlog::write("gameplay: model %s (%p)", model, m);
    }
    if (!entity(g_eye)) {
        void *e = sdk::create_entity("npc_bullseye");
        if (e) {
            char hp[16];
            std::snprintf(hp, sizeof hp, "%d", kEyeHealth);
            sdk::set_keyvalue(e, "targetname", kEyeName);
            sdk::set_keyvalue(e, "health", hp);
            sdk::set_keyvalue(e, "minangle", "360");
            sdk::set_keyvalue(e, "mindist", "0");
            sdk::set_keyvalue(e, "origin", origin);
            sdk::dispatch_spawn(e);
            g_eye = sdk::handle_of(e);
        }
        void *r = sdk::create_entity("ai_relationship");
        if (r) {
            sdk::set_keyvalue(r, "targetname", kRelName);
            sdk::set_keyvalue(r, "subject", "npc_portal_turret_floor");
            sdk::set_keyvalue(r, "target", kEyeName);
            sdk::set_keyvalue(r, "disposition", "1");  // hate
            sdk::set_keyvalue(r, "rank", "99");
            sdk::set_keyvalue(r, "StartActive", "1");
            sdk::set_keyvalue(r, "Reciprocal", "0");
            sdk::dispatch_spawn(r);
            g_rel = sdk::handle_of(r);
            sdk::execf("ent_fire %s ApplyRelationship", kRelName);
        }
        fpvlog::write("gameplay: bullseye %p, relationship %p", e, r);
    }
}

// --- portals from the drone -------------------------------------------------------------------

using TraceFirePortalFn = int(__thiscall *)(void *gun, const Vector &origin, const Vector &dir, bool orange, int placed_by,
                                             void *info);
using FindPortalFn = void *(__cdecl *)(char linkage, bool orange, bool create);
TraceFirePortalFn g_trace_fire_portal = nullptr;
FindPortalFn g_find_portal = nullptr;

void find_portal_functions() {
    static bool done = false;
    if (done) return;
    done = true;
    // SourceAutoRecord's patterns for the current Portal 2 (CWeaponPortalgun::TraceFirePortal, FindPortal)
    g_trace_fire_portal = reinterpret_cast<TraceFirePortalFn>(sdk::scan(
        "server.dll", "53 8B DC 83 EC 08 83 E4 F0 83 C4 04 55 8B 6B 04 89 6C 24 04 8B EC 81 EC 38 07 00 00 56 57 8B F1"));
    g_find_portal = reinterpret_cast<FindPortalFn>(sdk::scan(
        "server.dll",
        "55 8B EC 0F B6 45 08 8D 0C 80 03 C9 53 8B 9C 09 ? ? ? ? 03 C9 56 57 85 DB 74 3C 8B B9 ? ? ? ? 33 C0 33 F6 EB 08"));
    fpvlog::write("gameplay: TraceFirePortal %p, FindPortal %p", reinterpret_cast<void *>(g_trace_fire_portal),
                  reinterpret_cast<void *>(g_find_portal));
}

// Fires a portal along the drone's camera, by the portal gun's own rules
// (portalable surfaces, fizzlers, room to fit), as far as Chell's gun can.
void fire_portal(const fpv_frame &f, bool orange) {
    void *gun = sdk::entity_from_handle(sdk::get<uint32_t>(sdk::player(), "m_hActiveWeapon", 0xFFFFFFFFu));
    if (!gun || std::strcmp(sdk::classname(gun), "weapon_portalgun") != 0) {
        g_status = "no portal gun";
        return;
    }
    if (!sdk::get<bool>(gun, orange ? "m_bCanFirePortal2" : "m_bCanFirePortal1")) {
        g_status = orange ? "the gun has no orange portal yet" : "the gun has no blue portal yet";
        return;
    }
    find_portal_functions();
    if (!g_trace_fire_portal || !g_find_portal) return;
    uint8_t linkage = sdk::get<uint8_t>(gun, "m_iPortalLinkageGroupID", 0);
    // TraceFirePortal needs both portal entities to exist (as SourceAutoRecord does)
    for (bool o : {false, true}) {
        const char *field = o ? "m_hSecondaryPortal" : "m_hPrimaryPortal";
        uint32_t *h = sdk::field<uint32_t>(gun, field);
        if (h && !sdk::entity_from_handle(*h)) {
            void *p = g_find_portal(char(linkage), o, true);
            if (p) *h = sdk::handle_of(p);
        }
    }
    // TracePortalPlacementInfo_t: result, final position, final angles, helper, trace
    alignas(16) uint8_t info[512] = {};
    Vector origin(f.camera.position), dir(f.camera.forward);
    {
        EyeNotSolid pass;
        g_trace_fire_portal(gun, origin, dir, orange, 2, info);
    }
    int result = *reinterpret_cast<int *>(info);
    Vector pos = *reinterpret_cast<Vector *>(info + 4);
    QAngle ang = *reinterpret_cast<QAngle *>(info + 16);
    sound(orange ? "Weapon_Portalgun.fire_red" : "Weapon_Portalgun.fire_blue");
    {
        const auto &tr = *reinterpret_cast<const sdk::CGameTrace *>(info + 32);
        fpvlog::write("gameplay: fire %s from %.0f %.0f %.0f dir %.2f %.2f %.2f: result %d at %.0f %.0f %.0f; trace %.0f %.0f %.0f "
                      "-> %.0f %.0f %.0f frac %.2f startsolid %d contents %x surface '%s' ent %s",
                      orange ? "orange" : "blue", origin.x, origin.y, origin.z, dir.x, dir.y, dir.z, result, pos.x, pos.y,
                      pos.z, tr.startpos.x, tr.startpos.y, tr.startpos.z, tr.endpos.x, tr.endpos.y, tr.endpos.z,
                      tr.fraction, int(tr.startsolid), tr.contents,
                      tr.surface.name && sdk::readable_str(tr.surface.name) ? tr.surface.name : "?",
                      tr.ent ? sdk::classname(tr.ent) : "-");
    }
    if (result > 2) {  // CANT_FIT, CLEANSER, OVERLAP_*, INVALID_*, PASSTHROUGH
        sound("Portal.fizzle_invalid_surface");
        g_status = "portal: no place (" + std::to_string(result) + ")";
        return;
    }
    sdk::uncheat("portal_place");
    sdk::execf("portal_place %d %d %.3f %.3f %.3f %.3f %.3f %.3f", int(linkage), orange ? 1 : 0, pos.x, pos.y, pos.z,
               ang.pitch, ang.yaw, ang.roll);
    g_status = orange ? "orange portal" : "blue portal";
}

// --- use -----------------------------------------------------------------------------------

constexpr float kUseRange = 96;  // units ahead of the camera

bool usable(const char *cls) {
    for (const char *u : {"prop_button", "prop_under_button", "func_button", "func_rot_button", "prop_floor_button"})
        if (std::strcmp(cls, u) == 0) return true;
    return false;
}

void use(const fpv_frame &f) {
    Vector from(f.camera.position), dir(f.camera.forward);
    sdk::TraceFilter filter;
    add_own_entities(filter);
    sdk::CGameTrace tr;
    sdk::trace_ray(from, from + dir * kUseRange, sdk::MASK_SOLID, &filter, tr);
    void *hit = tr.fraction < 1 ? tr.ent : nullptr;
    void *target = hit && usable(sdk::classname(hit)) ? hit : nullptr;
    if (!target) {  // a button next to where the look ends (the plate under a pedestal button)
        Vector end = tr.endpos;
        float best = 40;
        for (void *e : sdk::entities()) {
            if (!usable(sdk::classname(e))) continue;
            float d = (sdk::abs_origin(e) - end).length();
            if (d < best) best = d, target = e;
        }
    }
    if (!target) {
        sound("Portal.Button_Locked");
        g_status = "use: nothing to use";
        return;
    }
    Vector o = sdk::abs_origin(target);
    sdk::execf("script p2fpv_fire_nearest(\"%s\", %.2f, %.2f, %.2f, \"Press\", \"\")", sdk::classname(target), o.x, o.y,
               o.z);
    g_status = std::string("use: ") + sdk::classname(target);
}

// --- grab ----------------------------------------------------------------------------------

bool grabbable(const char *cls) {
    for (const char *c : {"prop_weighted_cube", "prop_monster_box", "npc_portal_turret_floor", "prop_physics",
                          "prop_physics_override", "prop_physics_paintable"})
        if (std::strcmp(cls, c) == 0) return true;
    return false;
}

float entity_radius(void *e) {
    Vector lo = sdk::get<Vector>(e, "m_Collision.m_vecMins"), hi = sdk::get<Vector>(e, "m_Collision.m_vecMaxs");
    float r = (hi - lo).length() * 0.5f;
    return r > 1 ? r : 16;
}

void drop(bool fizzled = false) {
    if (!g_cargo.active) return;
    g_cargo.active = false;
    if (drone::handle()) fpv.fpv_drone_set_cargo(drone::handle(), nullptr);
    void *e = entity(g_cargo.handle);
    if (!e) return;
    Vector o = sdk::abs_origin(e);
    const char *cls = sdk::classname(e);
    if (fizzled) {
        sdk::execf("script p2fpv_fire_nearest(\"%s\", %.2f, %.2f, %.2f, \"Dissolve\", \"\")", cls, o.x, o.y, o.z);
    } else {
        sdk::execf("script p2fpv_fire_nearest(\"%s\", %.2f, %.2f, %.2f, \"EnableMotion\", \"\")", cls, o.x, o.y, o.z);
        sdk::execf("script p2fpv_velocity_nearest(\"%s\", %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, false)", cls, o.x, o.y,
                   o.z, g_cargo.vel.x, g_cargo.vel.y, g_cargo.vel.z);
    }
    g_status = fizzled ? "cargo fizzled" : "dropped";
    fpvlog::write("gameplay: %s %s", g_status.c_str(), cls);
}

void grab(const fpv_frame &f) {
    Vector at(f.position);
    void *best = nullptr;
    float best_gap = kGrabReach;
    for (void *e : sdk::entities()) {
        if (!grabbable(sdk::classname(e))) continue;
        float gap = (sdk::abs_origin(e) - at).length() - entity_radius(e);
        if (gap < best_gap) best_gap = gap, best = e;
    }
    if (!best) {
        sound("Portal.Button_Locked");
        g_status = "grab: nothing in reach";
        return;
    }
    Vector o = sdk::abs_origin(best);
    // the cube's centre (a prop's origin may be at its base)
    Vector lo = sdk::get<Vector>(best, "m_Collision.m_vecMins"), hi = sdk::get<Vector>(best, "m_Collision.m_vecMaxs");
    Vector centre = o + (lo + hi) * 0.5f;
    // a sphere as wide as the cube's half side: it rests on its faces, not its corners
    float half = std::min({hi.x - lo.x, hi.y - lo.y, hi.z - lo.z}) * 0.5f;
    if (half < 4) half = entity_radius(best) * 0.6f;
    g_cargo.handle = sdk::handle_of(best);
    float drone_yaw = sdk::vectors_angles(qrot(f.rotation, {1, 0, 0}), qrot(f.rotation, {0, 0, 1})).yaw;
    QAngle a = sdk::abs_angles(best);
    g_cargo.yaw = a.yaw - drone_yaw;
    sdk::angle_vectors(a, nullptr, nullptr, &g_cargo.up);
    g_cargo.pos = centre;
    g_cargo.vel = Vector{};
    g_cargo.active = true;
    fpv_cargo c;
    fpv.fpv_cargo_defaults(&c);
    c.mass = kCargoMassKg;
    qrot_inv(f.rotation, centre - at).to(c.offset);
    c.radius = half;
    c.line_length = half + kLineSlack;
    fpv.fpv_drone_set_cargo(drone::handle(), &c);
    const char *cls = sdk::classname(best);
    sdk::execf("script p2fpv_fire_nearest(\"%s\", %.2f, %.2f, %.2f, \"DisableMotion\", \"\")", cls, o.x, o.y, o.z);
    g_status = std::string("carrying ") + cls;
    fpvlog::write("gameplay: grabbed %s, %.0f units away, sphere %.1f", cls, (centre - at).length(), half);
}

// Where the cargo is now (libfpv's body on the line), upright along the line.
bool cargo_pose(Vector &pos, QAngle &ang) {
    if (!g_cargo.active || !g_pinned_valid) return false;
    void *e = entity(g_cargo.handle);
    if (!e) return false;
    // libfpv's position is the cube's centre; the entity's origin may be off it
    Vector lo = sdk::get<Vector>(e, "m_Collision.m_vecMins"), hi = sdk::get<Vector>(e, "m_Collision.m_vecMaxs");
    Vector mid = (lo + hi) * 0.5f;
    Vector up = g_cargo.up.normalized();
    float drone_yaw = sdk::vectors_angles(qrot(g_drone_q, {1, 0, 0}), qrot(g_drone_q, {0, 0, 1})).yaw;
    Vector f;
    sdk::angle_vectors({0, drone_yaw + g_cargo.yaw, 0}, &f, nullptr, nullptr);
    f = (f - up * f.dot(up)).normalized();
    if (f.length() < 0.5f) f = up.cross({0, 1, 0}).normalized();
    ang = sdk::vectors_angles(f, up);
    Vector r = f.cross(up);  // Source's right of forward / up
    pos = g_cargo.pos - (f * mid.x - r * mid.y + up * mid.z);
    return true;
}

// --- reactions to the drone's impacts ----------------------------------------------------------

constexpr float kBumpSpeed = 3.0f * kUnitsPerMeter;  // units/s

void impacts(const fpv_frame &f) {
    if (f.impact_count && g_fling.t > 0.15f) g_fling.active = false;  // it landed or hit something (not the plate)
    for (uint32_t i = 0; i < f.impact_count; i++) {
        const fpv_impact &im = f.impacts[i];
        if (im.speed < kBumpSpeed) continue;
        void *e = world::object_entity(im.object_id);
        uint32_t h = e ? sdk::handle_of(e) : 0;
        if (g_cooldown[h] > 0) continue;
        g_cooldown[h] = 0.4f;
        sdk::execf("script p2fpv_sound(\"%s\", \"SolidMetal.ImpactSoft\")", kModelName);
        if (!e || h == g_cargo.handle) continue;
        const char *cls = sdk::classname(e);
        if (!grabbable(cls)) continue;
        // shove what was hit: the drone's momentum into it (it's light, so a nudge;
        // a turret knocked over stops shooting, as when Chell does it)
        Vector n(im.normal), push = -n * (im.speed * 0.6f);
        Vector o = sdk::abs_origin(e);
        sdk::execf("script p2fpv_velocity_nearest(\"%s\", %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, true)", cls, o.x, o.y, o.z,
                   push.x, push.y, push.z + 40);
    }
}

// --- the chamber's hazards (server ticks) ----------------------------------------------------

bool segment_hits_box(const Vector &a, const Vector &b, const Vector &lo, const Vector &hi) {
    float t0 = 0, t1 = 1;
    const float A[3] = {a.x, a.y, a.z}, B[3] = {b.x, b.y, b.z}, L[3] = {lo.x, lo.y, lo.z}, H[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 3; k++) {
        float d = B[k] - A[k];
        if (std::fabs(d) < 1e-6f) {
            if (A[k] < L[k] || A[k] > H[k]) return false;
            continue;
        }
        float u0 = (L[k] - A[k]) / d, u1 = (H[k] - A[k]) / d;
        if (u0 > u1) std::swap(u0, u1);
        t0 = std::max(t0, u0);
        t1 = std::min(t1, u1);
        if (t0 > t1) return false;
    }
    return true;
}

bool enabled(void *e) { return !sdk::get<bool>(e, "m_bDisabled", false); }

// Faith plates: Valve's trigger_catapult aims at its target at a fixed speed
// (time = distance / speed, plus the gravity it loses on the way).
Vector catapult_velocity(void *cat, const Vector &from, float *duration) {
    float speed = sdk::get<float>(cat, "m_flPhysicsVelocity", 0);
    if (speed <= 0) speed = sdk::get<float>(cat, "m_flPlayerVelocity", 0);
    if (speed <= 0) speed = 600;
    void *target = sdk::entity_from_handle(sdk::get<uint32_t>(cat, "m_hLaunchTarget", 0xFFFFFFFFu));
    if (!target) {
        Vector f;
        sdk::angle_vectors(sdk::get<QAngle>(cat, "m_vecLaunchAngles"), &f, nullptr, nullptr);
        *duration = 0;
        return f * speed;
    }
    Vector d = sdk::abs_origin(target) - from;
    float t = d.length() / speed;
    *duration = t;
    Vector v = d * (1.0f / std::max(t, 0.05f));
    float g = 9.81f * kUnitsPerMeter;  // libfpv's gravity (the real one), not sv_gravity's 600
    v.z += 0.5f * g * t;
    return v;
}

void hazards(float dt) {
    fpv_drone *d = drone::handle();
    if (!d || !g_pinned_valid) return;
    Vector now = g_drone_pos, before = g_have_prev ? g_prev_pos : now;
    g_prev_pos = now;
    g_have_prev = true;
    for (void *e : sdk::entities()) {
        const char *cls = sdk::classname(e);
        if (cls[0] != 't' && cls[0] != 'p' && cls[0] != 'e') continue;  // trigger_*, projected_*, env_*
        bool cleanser = std::strcmp(cls, "trigger_portal_cleanser") == 0;
        bool hurt = std::strcmp(cls, "trigger_hurt") == 0;
        bool catapult = std::strcmp(cls, "trigger_catapult") == 0;
        bool beam = std::strcmp(cls, "projected_tractor_beam_entity") == 0;
        bool laser = std::strcmp(cls, "env_portal_laser") == 0;
        if (!(cleanser || hurt || catapult || beam || laser)) continue;
        if (beam) {
            // an excursion funnel: carried along its axis, centred, weightless
            Vector a = sdk::get<Vector>(e, "m_vecStartPoint"), b = sdk::get<Vector>(e, "m_vecEndPoint");
            Vector axis = b - a;
            float len = axis.length();
            if (len < 1) continue;
            axis = axis * (1 / len);
            float along = (now - a).dot(axis);
            Vector closest = a + axis * along;
            float radius = 54;  // the funnel's
            if (along < 0 || along > len || (now - closest).length() > radius) continue;
            // its projector: the one nearest the start (a beam out of a portal has none
            // there, the nearest is still the one feeding it); a reversed one pulls back
            void *emitter = sdk::entity_from_handle(sdk::get<uint32_t>(e, "m_hOwnerEntity", 0xFFFFFFFFu));
            if (emitter && std::strcmp(sdk::classname(emitter), "prop_tractor_beam") != 0) emitter = nullptr;
            float best = 1e30f;
            for (void *p : sdk::entities()) {
                if (emitter) break;
                if (std::strcmp(sdk::classname(p), "prop_tractor_beam") != 0) continue;
                float dd = (sdk::abs_origin(p) - a).length();
                if (dd < best) best = dd, emitter = p;
            }
            float speed = emitter ? sdk::get<float>(emitter, "m_flLinearForce", 250) : 250;
            Vector want = axis * speed + (closest - now) * 2.0f;
            float k = 1 - std::exp(-4 * dt);
            Vector dv = (want - g_drone_vel) * k + Vector{0, 0, 9.81f * kUnitsPerMeter * dt};
            float v[3];
            dv.to(v);
            fpv.fpv_drone_push(d, v, 0);
            g_status = "in a funnel";
            continue;
        }
        if (laser) {
            if (!sdk::get<bool>(e, "m_bLaserOn", false)) continue;
            Vector a = sdk::get<Vector>(e, "m_vStartPoint"), b = sdk::get<Vector>(e, "m_vEndPoint", a);
            Vector ab = b - a;
            float l2 = ab.dot(ab);
            if (l2 < 1) continue;
            float t = std::clamp((now - a).dot(ab) / l2, 0.0f, 1.0f);
            if ((a + ab * t - now).length() < 8) {
                fpv.fpv_drone_damage_props(d, -1, 0.8f * dt);
                g_status = "in a laser";
            }
            continue;
        }
        Vector lo, hi;
        if (!enabled(e) || !sdk::world_bounds(e, lo, hi)) continue;
        if (!segment_hits_box(before, now, lo, hi)) continue;
        if (cleanser) {
            g_fizzle = true;
        } else if (hurt) {
            // goo's trigger_hurt kills Chell outright (its damage is in the hundreds):
            // the drone is lost too; a weaker one wears the props down
            if (sdk::get<float>(e, "m_flDamage", 0) >= 50) {
                g_drowned = true;
            } else {
                fpv.fpv_drone_damage_props(d, -1, 0.5f * dt);
                g_status = "hurt";
            }
        } else if (catapult) {
            uint32_t h = sdk::handle_of(e);
            if (g_cooldown[h] > 0) continue;
            g_cooldown[h] = 1.0f;
            float v[3], duration = 0;
            Vector v0 = catapult_velocity(e, now, &duration);
            v0.to(v);
            fpv.fpv_drone_push(d, v, 1);
            g_fling = Fling{duration > 0, v0, 0, duration};
            g_status = "launched by a faith plate";
            fpvlog::write("gameplay: catapult %s -> %.0f %.0f %.0f", sdk::targetname(e), v[0], v[1], v[2]);
        }
    }
}

// Turrets shoot the bullseye that follows the drone: its lost health is the props'.
void turret_damage() {
    void *eye = entity(g_eye);
    fpv_drone *d = drone::handle();
    if (!eye || !d) return;
    int hp = sdk::get<int>(eye, "m_iHealth", kEyeHealth);
    if (hp >= kEyeHealth) return;
    sdk::execf("script p2fpv_health(\"%s\", %d)", kEyeName, kEyeHealth);
    hit(kEyeHealth - hp);
}

}  // namespace

void hit(int damage) {
    fpv_drone *d = drone::handle();
    if (!d || damage <= 0) return;
    // a turret's burst is about 3 damage per bullet: a dozen bullets break a prop
    fpv.fpv_drone_damage_props(d, int(g_server_time * 1000) % 4, std::min(1.0f, damage * 0.03f));
    g_status = "shot";
}

// --- public ---------------------------------------------------------------------------------

void declare_controls() {
    fpv_control_desc c[4];
    std::memset(c, 0, sizeof c);
    struct D { uint32_t id; const char *name, *label; uint32_t kind; int key, pad; } const defs[] = {
        {kFireBlue, "portal2.fire_blue", "Fire blue portal", FPV_CONTROL_INSTANT, '1', FPV_GAMEPAD_LEFT_TRIGGER},
        {kFireOrange, "portal2.fire_orange", "Fire orange portal", FPV_CONTROL_INSTANT, '2', FPV_GAMEPAD_RIGHT_TRIGGER},
        {kUse, "portal2.use", "Use (press a button)", FPV_CONTROL_INSTANT, 'E', FPV_GAMEPAD_WEST},
        {kGrab, "portal2.grab", "Grab / drop a cube", FPV_CONTROL_SWITCH, 'G', FPV_GAMEPAD_LEFT_SHOULDER},
    };
    for (int i = 0; i < 4; i++) {
        c[i].struct_size = sizeof c[i];
        c[i].id = defs[i].id;
        c[i].name = defs[i].name;
        c[i].label = defs[i].label;
        c[i].kind = defs[i].kind;
        c[i].key = defs[i].key;
        c[i].gamepad_button = defs[i].pad;
    }
    if (fpv.fpv_controls_set(c, 4) != FPV_OK) fpvlog::write("gameplay: controls_set: %s", fpv.fpv_last_error());
}

void on_launch(const float pos[3]) {
    install_script();
    spawn_entities(Vector(pos));
    g_have_prev = false;
    g_pinned_valid = false;
    g_fizzle = false;
    g_drowned = false;
    g_wrecked = false;
    g_passes_seen = 0;
    g_cargo = Carried{};
    g_fling = Fling{};
    g_status = "launched";
    // a switch left on at launch is no grab
    uint32_t presses;
    for (uint32_t id : {kFireBlue, kFireOrange, kUse, kGrab}) fpv.fpv_control_poll(id, &presses);
}

void on_teleport() {
    g_fling = Fling{};
    g_have_prev = false;
    g_pinned_valid = false;  // the server ticks wait for the new pose
}

void on_respawn() {
    drop();
    g_fling = Fling{};
    g_pinned_valid = false;  // the server ticks wait for the new pose
    g_drowned = false;
    g_have_prev = false;
    g_passes_seen = 0;
}

void on_stop() {
    drop();
    remove_entity(g_model);
    remove_entity(g_eye);
    remove_entity(g_rel);
    g_pinned_valid = false;
    g_status = "idle";
}

void level_shutdown() {
    g_model = g_eye = g_rel = 0xFFFFFFFFu;
    g_cargo = Carried{};
    g_cooldown.clear();
    g_pinned_valid = false;
}

void server_activate() {
    sdk::precache_model(model_path());
}

void add_own_entities(sdk::TraceFilter &filter) {
    filter.add(entity(g_model));
    filter.add(entity(g_eye));
    filter.add(sdk::player());  // hidden while a drone flies, and maybe standing in right where it is
    if (g_cargo.active) filter.add(entity(g_cargo.handle));
}

namespace {
uint32_t g_pressed_blue = 0, g_pressed_orange = 0, g_pressed_use = 0;
int g_forced_grab = -1;  // the autotest's grab switch: -1 the radio's, 0 off, 1 on
}  // namespace

void press(Action a) {
    switch (a) {
    case Action::FireBlue: g_pressed_blue++; break;
    case Action::FireOrange: g_pressed_orange++; break;
    case Action::Use: g_pressed_use++; break;
    case Action::GrabOn: g_forced_grab = 1; break;
    case Action::GrabOff: g_forced_grab = 0; break;
    }
}

void client_tick(const fpv_frame &f, float dt, bool mouse) {
    g_drone_pos = Vector(f.position);
    g_drone_vel = Vector(f.velocity);
    std::memcpy(g_drone_q, f.rotation, sizeof g_drone_q);
    g_pinned_valid = true;
    if (g_cargo.active && (f.cargo_flags & FPV_CARGO_CARRIED)) {
        g_cargo.pos = Vector(f.cargo_position);
        g_cargo.vel = Vector(f.cargo_velocity);
        // upright along a taut line, back to upright when it is slack and lifted; as it lay on the ground
        Vector want = g_cargo.up;
        Vector line = g_drone_pos - g_cargo.pos;
        if (f.cargo_flags & FPV_CARGO_LINE_TAUT) want = line.normalized();
        float k = std::min(1.0f, dt * 8);
        Vector up = g_cargo.up + (want - g_cargo.up) * k;
        if (up.length() > 0.1f) g_cargo.up = up.normalized();
    }

    uint32_t blue = 0, orange = 0, use_n = 0, grab_n = 0;
    fpv.fpv_control_poll(kFireBlue, &blue);
    fpv.fpv_control_poll(kFireOrange, &orange);
    fpv.fpv_control_poll(kUse, &use_n);
    bool grab_on = fpv.fpv_control_poll(kGrab, &grab_n) != 0;
    blue += g_pressed_blue, orange += g_pressed_orange, use_n += g_pressed_use;
    g_pressed_blue = g_pressed_orange = g_pressed_use = 0;
    if (g_forced_grab >= 0) {
        if (g_forced_grab && !grab_on) grab_n++;
        grab_on = g_forced_grab != 0;
    }
    static bool lmb = false, rmb = false;
    bool l = mouse && (GetAsyncKeyState(VK_LBUTTON) & 0x8000), r = mouse && (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
    if (l && !lmb) blue++;
    if (r && !rmb) orange++;
    lmb = l, rmb = r;
    if (blue) fire_portal(f, false);
    if (orange) fire_portal(f, true);
    if (use_n) use(f);
    if (grab_on && !g_cargo.active && grab_n) grab(f);
    if (!grab_on && g_cargo.active) drop();
    if (g_cargo.active && !entity(g_cargo.handle)) {  // it fizzled or broke on its own
        g_cargo.active = false;
        fpv.fpv_drone_set_cargo(drone::handle(), nullptr);
    }

    if (kamikaze::wrecked(f) != g_wrecked) {
        g_wrecked = !g_wrecked;
        if (g_wrecked) drop();
        sdk::execf("ent_fire %s %s", kModelName, g_wrecked ? "DisableDraw" : "EnableDraw");
        // the blast hurt the turrets' target too: not the next drone's damage
        sdk::execf("script p2fpv_health(\"%s\", %d)", kEyeName, kEyeHealth);
    }
    if (g_wrecked) return;

    if (f.portal_passes != g_passes_seen) {
        g_passes_seen = f.portal_passes;
        g_fling.active = false;  // the arc was on this side
        sound("PortalPlayer.EnterPortal");
    }
    impacts(f);
    for (auto &kv : g_cooldown) kv.second -= dt;
    // out of the world (a pit with no floor, or through a gap in the brushes)
    if (f.position[2] < pilot::spot().z - 4000) g_drowned = true;
    if (g_drowned) {
        g_drowned = false;
        fpvlog::write("gameplay: lost (goo or out of the world) at %.0f %.0f %.0f", f.position[0], f.position[1],
                      f.position[2]);
        sound("Physics.WaterSplash");
        drop(true);
        drone::respawn();
        g_status = "lost in the goo";
    }
    if (g_fizzle) {
        g_fizzle = false;
        fpvlog::write("gameplay: fizzled");
        sound("Prop.Fizzled");
        drop(true);
        drone::respawn();
        g_status = "fizzled";
    }
}

void server_tick() {
    if (!drone::handle() || !g_pinned_valid || sdk::is_paused() || g_wrecked) return;
    float dt = sdk::I.globals ? sdk::I.globals->interval_per_tick : 1.0f / 60;
    g_server_time += dt;
    // the bullseye (behind the lens) and the model where the drone is, for the
    // server: turrets, lasers, the PVS; the client is pinned every frame
    Vector fwd = qrot(g_drone_q, {1, 0, 0});
    QAngle a = model_angles(g_drone_q);
    Vector eye = g_drone_pos - fwd * kEyeBehind;
    sdk::execf("script p2fpv_place(\"%s\", %.2f, %.2f, %.2f, 0, 0, 0)", kEyeName, eye.x, eye.y, eye.z);
    sdk::execf("script p2fpv_place(\"%s\", %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)", kModelName, g_drone_pos.x, g_drone_pos.y,
               g_drone_pos.z, a.pitch, a.yaw, a.roll);
    Vector cp;
    QAngle ca;
    if (cargo_pose(cp, ca)) {
        if (void *e = entity(g_cargo.handle)) {
            Vector o = sdk::abs_origin(e);
            sdk::execf("script p2fpv_place_nearest(\"%s\", %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f, %.2f)",
                       sdk::classname(e), o.x, o.y, o.z, cp.x, cp.y, cp.z, ca.pitch, ca.yaw, ca.roll);
        }
    }
    if (g_fling.active) {
        g_fling.t += dt;
        if (g_fling.t > g_fling.duration) {
            g_fling.active = false;
        } else {
            Vector v = g_fling.v0 + Vector{0, 0, -9.81f * kUnitsPerMeter * g_fling.t};
            float vv[3];
            v.to(vv);
            fpv.fpv_drone_push(drone::handle(), vv, 1);
        }
    }
    hazards(dt);
    turret_damage();
    pilot::server_tick();
    static std::string logged;
    if (g_status != logged) logged = g_status, fpvlog::write("gameplay: status '%s'", g_status.c_str());
}

void pin_visuals(const fpv_frame &f) {
    // The networked positions trail the drone by a tick plus interpolation:
    // put the client's copies where the drone is this frame.
    auto pin = [](uint32_t h, const Vector &pos, const QAngle &ang) {
        void *server = sdk::entity_from_handle(h);
        if (!server) return;
        void *c = sdk::client_entity(sdk::entity_index(server));
        if (!c) return;
        for (const char *name : {"m_vecOrigin", "m_vecAbsOrigin", "m_vecNetworkOrigin"}) {
            int off = sdk::client_field_offset(c, name);
            if (off >= 0) *reinterpret_cast<Vector *>(static_cast<char *>(c) + off) = pos;
        }
        for (const char *name : {"m_angRotation", "m_angAbsRotation", "m_angNetworkAngles"}) {
            int off = sdk::client_field_offset(c, name);
            if (off >= 0) *reinterpret_cast<QAngle *>(static_cast<char *>(c) + off) = ang;
        }
    };
    pin(g_model, Vector(f.position), model_angles(f.rotation));
    // the props: blades when they stand still, a blurred disc when they spin
    if (void *server = sdk::entity_from_handle(g_model)) {
        if (void *c = sdk::client_entity(sdk::entity_index(server))) {
            int off = sdk::client_field_offset(c, "m_nBody");
            float rpm = 0;
            for (const fpv_motor_state &m : f.motors) rpm = std::max(rpm, m.rpm);
            if (off >= 0) *reinterpret_cast<int *>(static_cast<char *>(c) + off) = rpm > 1500 ? 1 : 0;
        }
    }
    Vector cp;
    QAngle ca;
    if (cargo_pose(cp, ca)) pin(g_cargo.handle, cp, ca);
}

const char *status() { return g_status.c_str(); }

}  // namespace gameplay
