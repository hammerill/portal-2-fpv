#include "drone.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "audio.h"
#include "crash.h"
#include "fpvapi.h"
#include "gameplay.h"
#include "kamikaze.h"
#include "log.h"
#include "pilot.h"
#include "platform.h"
#include "render.h"
#include "world.h"

namespace drone {
namespace {

using fpvapi::fpv;
using sdk::QAngle;
using sdk::Vector;

constexpr uint32_t kMoveChellAction = 1;   // F8 > Fly button
constexpr float kLaunchDistance = 40.0f;   // units in front of Chell's feet
constexpr double kWarmup = 0.05;           // s of sim time before the frames are trusted

fpv_log_fn g_log = nullptr;
fpv_drone *g_drone = nullptr;
fpv_frame g_frame{};
bool g_have_frame = false;
double g_first_sim = -1;
bool g_warm = false;
View g_view = View::Fpv;
bool g_pilot = false;        // F: back to Chell, the drone left in the world
bool g_hud_hidden = false;   // our convars are set for a drone view
QAngle g_hold_angles;        // Chell's view while she stands still
float g_age = 0;
double g_last = 0;

std::mutex g_snap_mutex;
fpv_frame g_snap{};
bool g_snap_fpv = false, g_snap_no_signal = false;

// --- keys -------------------------------------------------------------------------------

using input::Key;

bool game_has_focus() { return input::focused(); }

// The drone's keyboard (libfpv's fallback sticks and switches, the overlay's
// keys and the game controls), by physical position for the letters. In the
// pilot view the letters are Chell's, so libfpv sees them released, unless
// the overlay is open and wants them.
void forward_keys() {
    struct K { Key key; int fpv; bool flight; };
    static const K keys[] = {
        {Key::W, 'W', true}, {Key::A, 'A', true}, {Key::S, 'S', true}, {Key::D, 'D', true},
        {Key::T, 'T', true}, {Key::B, 'B', true}, {Key::E, 'E', true}, {Key::G, 'G', true},
        {Key::D1, '1', true}, {Key::D2, '2', true},
        {Key::Enter, FPV_KEY_ENTER, true}, {Key::Left, FPV_KEY_LEFT, true}, {Key::Right, FPV_KEY_RIGHT, true},
        {Key::Up, FPV_KEY_UP, true}, {Key::Down, FPV_KEY_DOWN, true}, {Key::F8, FPV_KEY_F8, false},
        {Key::Escape, FPV_KEY_ESCAPE, false}, {Key::Tab, FPV_KEY_TAB, false}, {Key::Backspace, FPV_KEY_BACKSPACE, false},
        {Key::Space, FPV_KEY_SPACE, false}, {Key::Delete, FPV_KEY_DELETE, false},
#if defined(LIBFPV_KAMIKAZE)
        {Key::K, 'K', true}, {Key::X, 'X', true},  // bomb arm, explode
#endif
    };
    bool flight_keys = (g_drone && !g_pilot) || fpv.fpv_overlay_wants_input();
    static bool was_down[sizeof(keys) / sizeof(keys[0])];
    static bool primed = false;  // a key already held when we start is not a press
    bool focus = game_has_focus();
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        bool down = focus && (flight_keys || !keys[i].flight) && input::key_down(keys[i].key);
        if (primed && down != was_down[i]) fpv.fpv_input_key(keys[i].fpv, down);
        was_down[i] = down;
    }
    primed = true;
}

// Edge-detected press; the first reading of a key is only a baseline.
bool key_pressed(Key k) {
    static bool was_down[int(Key::Count)], seen[int(Key::Count)];
    int i = int(k);
    bool down = game_has_focus() && input::key_down(k);
    bool pressed = seen[i] && down && !was_down[i];
    was_down[i] = down;
    seen[i] = true;
    return pressed;
}

// --- HUD and Chell -----------------------------------------------------------------------

void hide_hud(bool hide) {
    if (hide == g_hud_hidden) return;
    g_hud_hidden = hide;
    for (const char *c : {"cl_drawhud", "r_drawviewmodel", "crosshair"}) sdk::uncheat(c);
    // Chell herself is hidden for the whole flight (pilot.cpp)
    if (hide) sdk::exec("cl_drawhud 0; r_drawviewmodel 0; crosshair 0");
    else sdk::exec("cl_drawhud 1; r_drawviewmodel 1; crosshair 1");
}

bool chell_alive() {
    void *p = sdk::player();
    return p && sdk::get<int>(p, "m_iHealth", 100) > 0 && sdk::get<char>(p, "m_lifeState", 0) == 0;
}

Vector chell_feet() { return sdk::abs_origin(sdk::player()); }

Vector chell_eye() { return chell_feet() + sdk::get<Vector>(sdk::player(), "m_vecViewOffset", Vector{0, 0, 64}); }

// libfpv's rotation (host = Source axes) of a yaw about Z.
void yaw_quat(float yaw_deg, float q[4]) {
    float h = yaw_deg * 3.14159265f / 360.0f;
    q[0] = 0, q[1] = 0, q[2] = std::sin(h), q[3] = std::cos(h);
}

// The quaternion of an orientation given by Source forward / left / up:
// libfpv's rotation takes the host's right (-Y), up (Z) and forward (X).
void basis_quat(const Vector &f, const Vector &left, const Vector &u, float q[4]) {
    float m[3][3] = {{f.x, left.x, u.x}, {f.y, left.y, u.y}, {f.z, left.z, u.z}};  // columns: images of X, Y, Z
    float tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        float s = std::sqrt(tr + 1) * 2;
        q[3] = 0.25f * s, q[0] = (m[2][1] - m[1][2]) / s, q[1] = (m[0][2] - m[2][0]) / s, q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        float s = std::sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s, q[0] = 0.25f * s, q[1] = (m[0][1] + m[1][0]) / s, q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        float s = std::sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s, q[0] = (m[0][1] + m[1][0]) / s, q[1] = 0.25f * s, q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        float s = std::sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s, q[0] = (m[0][2] + m[2][0]) / s, q[1] = (m[1][2] + m[2][1]) / s, q[2] = 0.25f * s;
    }
}

fpv_view libfpv_view() {
    if (g_pilot) return FPV_VIEW_PILOT;
    return g_view == View::Chase ? FPV_VIEW_CHASE : g_view == View::Los ? FPV_VIEW_LOS : FPV_VIEW_FPV;
}

void report_view() {
    static fpv_view reported = FPV_VIEW_FPV;
    fpv_view v = libfpv_view();
    if (v != reported) {
        reported = v;
        fpv.fpv_set_view(v);
    }
}

// Where the drone launches: on the floor in front of the pilot, facing her way.
bool launch_pose(float pos[3], float rot[4]) {
    void *p = sdk::player();
    if (!p) return false;
    QAngle eye = sdk::view_angles();
    Vector f, r, u;
    sdk::angle_vectors({0, eye.yaw, 0}, &f, &r, &u);
    Vector feet = pilot::spot();
    Vector want = feet + f * kLaunchDistance + Vector{0, 0, 24};
    // not into a wall in front of her
    sdk::CGameTrace tr;
    sdk::TraceFilter filter;
    filter.add(p);
    sdk::trace_ray(feet + Vector{0, 0, 24}, want, sdk::MASK_SOLID, &filter, tr);
    if (tr.fraction < 1) want = feet + Vector{0, 0, 24} + f * std::max(0.0f, kLaunchDistance * tr.fraction - 12);
    sdk::trace_ray(want, want - Vector{0, 0, 200}, sdk::MASK_SOLID, &filter, tr);
    // a little above the floor: rays from a centre on the surface start in the solid
    // and miss it (the drone fell through the arrival elevator's floor)
    Vector at = (tr.fraction < 1 ? tr.endpos : feet) + Vector{0, 0, 4};
    at.to(pos);
    yaw_quat(eye.yaw, rot);
    return true;
}

void enter_drone_view() {
    g_pilot = false;
    g_hold_angles = sdk::view_angles();
    hide_hud(true);
}

void launch() {
    if (!fpvapi::load(g_log)) return;
    float pos[3], rot[4];
    crash::note("launch: pose");
    if (!launch_pose(pos, rot)) return;
    crash::note("launch: world::install");
    fpvlog::write("launch: world::install");
    world::install();
    fpv_drone_desc d;
    fpv.fpv_drone_desc_defaults(&d);
    std::memcpy(d.position, pos, sizeof d.position);
    std::memcpy(d.rotation, rot, sizeof d.rotation);
    chell_feet().to(d.pilot_position);
    crash::note("launch: fpv_drone_create");
    fpvlog::write("launch: fpv_drone_create");
    fpv_result r = fpv.fpv_drone_create(&d, &g_drone);
    if (r != FPV_OK) {
        fpvlog::write("drone: create failed: %s (%s)", fpv.fpv_result_string(r), fpv.fpv_last_error());
        g_drone = nullptr;
        return;
    }
    crash::note("launch: gps home");
    fpvlog::write("launch: gps home");
    fpv.fpv_gps_set_home(nullptr);  // home where it arms, as on a real quad
    g_have_frame = false;
    g_first_sim = -1;
    g_warm = false;
    g_age = 0;
    g_view = View::Fpv;
    crash::note("launch: view");
    fpvlog::write("launch: view");
    enter_drone_view();
    report_view();
    audio::start();
    crash::note("launch: gameplay::on_launch");
    fpvlog::write("launch: gameplay::on_launch");
    gameplay::on_launch(pos);
    pilot::begin();
    sdk::exec("playgamesound GameUI.ButtonClickSound");
    fpvlog::write("drone: launched at %.0f %.0f %.0f", pos[0], pos[1], pos[2]);
}

void back_to_drone() {
    enter_drone_view();
    report_view();
    fpv.fpv_audio_set_listener(nullptr, nullptr);
    sdk::exec("playgamesound GameUI.ButtonClickSound");
}

void to_pilot() {
    pilot::home();
    g_pilot = true;
    hide_hud(false);
    sdk::set_view_angles(g_hold_angles);
    report_view();
    sdk::exec("playgamesound GameUI.ButtonBackSound");
}

// Y: Chell to where the drone is, on the floor below it, facing its way; the flight ends.
void move_chell_here() {
    if (!g_have_frame) return;
    Vector at(g_frame.position);
    sdk::CGameTrace tr;
    sdk::TraceFilter filter;
    filter.add(sdk::player());
    gameplay::add_own_entities(filter);
    sdk::trace_ray(at + Vector{0, 0, 8}, at - Vector{0, 0, 400}, sdk::MASK_SOLID, &filter, tr);
    Vector feet = tr.fraction < 1 ? tr.endpos + Vector{0, 0, 2} : at;
    QAngle a = sdk::vectors_angles(Vector(g_frame.camera.forward), Vector(g_frame.camera.up));
    pilot::release();
    kamikaze::cancel();
    sdk::execf("script p2fpv_move_player(%.2f, %.2f, %.2f)", feet.x, feet.y, feet.z);
    stop();
    sdk::set_view_angles({0, a.yaw, 0});
    fpvlog::write("drone: Chell moved to %.0f %.0f %.0f", feet.x, feet.y, feet.z);
}

void update_listener() {
    if (!g_pilot) return;
    Vector eye = chell_eye(), f, r, u;
    sdk::angle_vectors(sdk::view_angles(), &f, &r, &u);
    float pos[3], q[4];
    eye.to(pos);
    basis_quat(f, -r, u, q);
    fpv.fpv_audio_set_listener(pos, q);
}

// Vertical FOV for the real aspect (docs/INTEGRATION.md#coordinates-and-the-camera),
// then Source's horizontal FOV at 4:3, which the engine widens itself.
float source_fov(const fpv_camera &c, float aspect, bool lens) {
    float fov_y = c.fov_y_degrees;
    if (lens && (c.distortion_k1 != 0 || c.distortion_k2 != 0)) {
        auto edge = [&](float a) {
            float r2 = 1 / (1 + a * a);
            return (1 + c.distortion_k1 * r2 + c.distortion_k2 * r2 * r2) / (c.distortion_zoom > 0 ? c.distortion_zoom : 1);
        };
        float t = std::tan(fov_y * 3.14159265f / 360.0f) * edge(c.aspect_hint) / edge(aspect);
        fov_y = std::atan(t) * 360.0f / 3.14159265f;
    }
    fov_y = std::min(fov_y, 170.0f);
    float h43 = 2 * std::atan(std::tan(fov_y * 3.14159265f / 360.0f) * 4.0f / 3.0f) * 180.0f / 3.14159265f;
    return std::min(h43, 175.0f);
}

void apply_camera(sdk::CViewSetup *v) {
    View view = kamikaze::cutscene() ? View::Los : g_view;  // the blast, seen from the pilot
    const fpv_camera &c = view == View::Chase ? g_frame.chase_camera : view == View::Los ? g_frame.los_camera : g_frame.camera;
    Vector pos(c.position), f(c.forward), u(c.up);
    float aspect = v->height > 0 ? float(v->width) / float(v->height) : render::aspect();
    v->origin = pos;
    v->angles = sdk::vectors_angles(f, u);
    v->fov = source_fov(c, aspect, view == View::Fpv);
    v->zNear = view == View::Fpv ? world::near_plane(pos, f, u, aspect, c.fov_y_degrees) : 2.0f;
}

void snapshot() {
    std::lock_guard<std::mutex> lock(g_snap_mutex);
    g_snap_fpv = g_drone && g_have_frame && g_warm && !g_pilot && g_view == View::Fpv && !sdk::is_paused();
    if (g_snap_fpv) {
        g_snap = g_frame;
        g_snap_no_signal = g_frame.video.signal_strength <= 0.0f;
    }
}

}  // namespace

// --- public ------------------------------------------------------------------------------

void load(fpv_log_fn log) { g_log = log; }

void unload() {
    input::free_cursor(false);
    if (g_drone) stop();
    hide_hud(false);
}

void level_shutdown() {
    if (g_drone) {
        fpvlog::write("drone: level ends, flight stopped");
        stop();
    }
}

fpv_drone *handle() { return g_drone; }
const fpv_frame *frame() { return g_drone && g_have_frame ? &g_frame : nullptr; }
bool drone_view() { return g_drone && !g_pilot; }
View view() { return g_view; }
float age() { return g_age; }

bool load_libfpv() {
    if (fpvapi::loaded()) return true;
    if (!fpvapi::load(g_log)) return false;
    crash::note("declare_controls");
    gameplay::declare_controls();
    pilot::declare();
    kamikaze::declare();
    fpv.fpv_overlay_set_action(kMoveChellAction, "Move Chell here (Y)");
    crash::note("libfpv ready");
    return true;
}

void set_view(View v, bool pilot) {
    if (!g_drone) return;
    if (pilot) {
        if (!g_pilot) to_pilot();
        return;
    }
    g_view = v;
    if (g_pilot) back_to_drone();
    report_view();
}

void start() {
    if (!g_drone) launch();
}

void stop() {
    if (!g_drone) return;
    kamikaze::cancel();
    pilot::end();
    gameplay::on_stop();
    audio::stop();
    fpv.fpv_audio_set_listener(nullptr, nullptr);
    fpv.fpv_drone_delete(g_drone);
    g_drone = nullptr;
    g_have_frame = false;
    g_pilot = false;
    hide_hud(false);
    sdk::set_view_angles(g_hold_angles);
    fpv.fpv_set_view(FPV_VIEW_FPV);
    snapshot();
    fpvlog::write("drone: flight ended");
}

void respawn() {
    if (!g_drone) return;
    float pos[3], rot[4];
    if (!launch_pose(pos, rot)) return;  // in front of the spot, before she goes back to it
    kamikaze::cancel();
    pilot::home();
    gameplay::on_respawn();
    fpv.fpv_drone_repair(g_drone);
    fpv.fpv_drone_reset(g_drone, pos, rot);
    pilot::spot().to(pos);
    fpv.fpv_drone_set_pilot_position(g_drone, pos);
    g_first_sim = -1;
    g_warm = false;
    if (g_pilot) back_to_drone();
    fpvlog::write("drone: respawned at %.0f %.0f %.0f", pos[0], pos[1], pos[2]);
}

void respawn_at(const float pos[3], float yaw) {
    if (!g_drone) return;
    float rot[4];
    yaw_quat(yaw, rot);
    gameplay::on_teleport();
    fpv.fpv_drone_repair(g_drone);
    fpv.fpv_drone_reset(g_drone, pos, rot);
    g_first_sim = -1;
    g_warm = false;
    fpvlog::write("drone: put at %.0f %.0f %.0f yaw %.0f", pos[0], pos[1], pos[2], yaw);
}

void on_view(sdk::CViewSetup *view) {
    if (!sdk::in_game()) return;
    double now = platform::seconds();
    float dt = g_last > 0 ? float(now - g_last) : 0.016f;
    g_last = now;
    dt = std::min(dt, 0.25f);

    if (!fpvapi::loaded()) {
        bool f7 = key_pressed(Key::F7), f8 = key_pressed(Key::F8);
        if (f7 || f8) {
            fpvlog::write("%s pressed, loading libfpv", f7 ? "F7" : "F8");
            if (load_libfpv()) {
                if (f7) launch();
                else fpv.fpv_input_key(FPV_KEY_F8, 1), fpv.fpv_input_key(FPV_KEY_F8, 0);
            }
        }
        return;
    }
    forward_keys();
    // the overlay needs the cursor: the game stops reading (and re-centring) the mouse
    static bool mouse_freed = false;
    bool overlay_open = fpv.fpv_overlay_is_open() != 0;
    if (overlay_open != mouse_freed) {
        mouse_freed = overlay_open;
        sdk::uncheat("cl_mouseenable");
        sdk::exec(overlay_open ? "cl_mouseenable 0" : "cl_mouseenable 1");
        input::free_cursor(overlay_open);
    }
    bool move_chell = false;
    for (uint32_t id; (id = fpv.fpv_overlay_poll_action()) != 0;) move_chell |= id == kMoveChellAction;
    bool typing = fpv.fpv_overlay_wants_input() != 0;
    bool f7 = key_pressed(Key::F7), cam = key_pressed(Key::C), reset = key_pressed(Key::R), y = key_pressed(Key::Y),
         pilot = key_pressed(Key::F);
    if (!typing) {
        if (f7) {
            if (!g_drone) launch();
            else if (g_pilot) back_to_drone();
            else {
                stop();
                sdk::exec("playgamesound GameUI.ButtonBackSound");
            }
        } else if (g_drone) {
            move_chell |= y;
            if (pilot && !g_pilot) to_pilot();
            else if (cam && !g_pilot) {
                g_view = View((int(g_view) + 1) % int(View::Count));
                report_view();
                sdk::exec("playgamesound GameUI.ButtonMoveSound");
            }
            if (reset) respawn();
        }
    }
    if (g_drone && !chell_alive()) {
        fpvlog::write("drone: Chell died, flight stopped");
        stop();
    }
    if (g_drone && move_chell) move_chell_here();

    if (g_drone && !sdk::is_paused()) {
        float feet[3];
        pilot::spot().to(feet);
        fpv.fpv_drone_set_pilot_position(g_drone, feet);
        world::before_tick(dt);
        fpv_frame f{};
        f.struct_size = sizeof f;
        if (fpv.fpv_drone_tick(g_drone, dt, &f) == FPV_OK) {
            if (g_first_sim < 0) g_first_sim = f.sim_time;
            g_warm = g_warm || f.sim_time - g_first_sim >= kWarmup;
            if (g_warm) {
                g_frame = f;
                g_have_frame = true;
            }
        }
        g_age += dt;
        update_listener();
        // not on the frame from before a respawn (it is kept for the camera while the new one warms up)
        if (g_have_frame && g_warm) {
            gameplay::client_tick(g_frame, dt, drone_view() && !typing && game_has_focus());
            if (g_drone) pilot::frame(g_frame, drone_view());
            if (g_drone) kamikaze::frame(g_frame, dt);
        }
    }
    if (g_drone && g_have_frame) {
        if (!g_pilot) apply_camera(view);
        gameplay::pin_visuals(g_frame);
    }
    snapshot();
}

void on_create_move(sdk::CUserCmd *cmd) {
    if (!cmd) return;
    bool flying = g_drone && !g_pilot;
    bool typing = fpvapi::loaded() && fpv.fpv_overlay_wants_input();
    static bool held_on_foot = false;  // the overlay open while Chell is the player's
    if (!flying && !typing) {
        held_on_foot = false;
        return;
    }
    if (!flying && !held_on_foot) g_hold_angles = sdk::view_angles();  // where she looked when it opened
    held_on_foot = !flying;
    cmd->forwardmove = cmd->sidemove = cmd->upmove = 0;
    cmd->buttons = 0;
    cmd->impulse = 0;
    cmd->mousedx = cmd->mousedy = 0;
    cmd->viewangles = g_hold_angles;
    sdk::set_view_angles(g_hold_angles);
}

bool osd_frame(fpv_frame *out, bool *no_signal) {
    std::lock_guard<std::mutex> lock(g_snap_mutex);
    if (!g_snap_fpv) return false;
    *out = g_snap;
    *no_signal = g_snap_no_signal;
    return true;
}

}  // namespace drone
