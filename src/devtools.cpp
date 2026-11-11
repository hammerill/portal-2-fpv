#include "devtools.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "drone.h"
#include "fpvapi.h"
#include "gameplay.h"
#include "log.h"
#include "paths.h"
#include "pilot.h"
#include "sdk.h"
#include "world.h"

namespace devtools {
namespace {

using fpvapi::fpv;

bool read_file(const std::string &path, std::string &out) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[4096];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

void write_status() {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last < 500) return;
    last = now;
    std::string path = paths::data_dir() + "\\status.txt";
    FILE *f = std::fopen((path + ".tmp").c_str(), "w");
    if (!f) return;
    void *p = sdk::player();
    sdk::Vector feet = sdk::abs_origin(p);
    std::fprintf(f, "time %lu\nin_game %d paused %d\nchell %.1f %.1f %.1f health %d\n", now, int(sdk::in_game()),
                 int(sdk::is_paused()), feet.x, feet.y, feet.z, sdk::get<int>(p, "m_iHealth", -1));
    std::fprintf(f, "chell_state flags 0x%x movetype %d effects 0x%x takedamage %d\n", sdk::get<int>(p, "m_fFlags", -1),
                 int(sdk::get<uint8_t>(p, "m_MoveType", 0xFF)), sdk::get<int>(p, "m_fEffects", -1),
                 int(sdk::get<uint8_t>(p, "m_takedamage", 0xFF)));
    std::fprintf(f, "libfpv %d drone %d drone_view %d view %d age %.1f\n", int(fpvapi::loaded()), drone::handle() != nullptr,
                 int(drone::drone_view()), int(drone::view()), drone::age());
    if (const fpv_frame *fr = drone::frame()) {
        std::fprintf(f, "pos %.1f %.1f %.1f vel %.1f %.1f %.1f speed %.1f km/h\n", fr->position[0], fr->position[1],
                     fr->position[2], fr->velocity[0], fr->velocity[1], fr->velocity[2], fr->speed_kmh);
        std::fprintf(f, "flags 0x%x arming '%s' battery %.2f V signal %.2f portal_passes %u last %d\n", fr->flags,
                     fr->arming_disable_text, fr->battery.voltage, fr->video.signal_strength, fr->portal_passes,
                     fr->last_portal);
        std::fprintf(f, "camera %.1f %.1f %.1f fov %.1f k1 %.2f\n", fr->camera.position[0], fr->camera.position[1],
                     fr->camera.position[2], fr->camera.fov_y_degrees, fr->camera.distortion_k1);
        std::fprintf(f, "props %.2f %.2f %.2f %.2f rpm %.0f %.0f %.0f %.0f\n", fr->motors[0].prop_damage,
                     fr->motors[1].prop_damage, fr->motors[2].prop_damage, fr->motors[3].prop_damage, fr->motors[0].rpm,
                     fr->motors[1].rpm, fr->motors[2].rpm, fr->motors[3].rpm);
        fpv_input_status in{};
        in.struct_size = sizeof in;
        fpv.fpv_input_get_status(&in);
        std::fprintf(f, "input %d needs_setup %d device '%s'\n", int(in.source), int(in.needs_setup), in.device_name);
    }
    world::PortalPair pairs[8];
    int n = world::portal_pairs(pairs, 8);
    std::fprintf(f, "portal_pairs %d\n", n);
    for (int i = 0; i < n; i++) {
        sdk::Vector a = sdk::abs_origin(pairs[i].a), b = sdk::abs_origin(pairs[i].b);
        sdk::QAngle aa = sdk::abs_angles(pairs[i].a), ba = sdk::abs_angles(pairs[i].b);
        std::fprintf(f, "  %.1f %.1f %.1f (%.0f %.0f %.0f) <-> %.1f %.1f %.1f (%.0f %.0f %.0f)\n", a.x, a.y, a.z, aa.pitch,
                     aa.yaw, aa.roll, b.x, b.y, b.z, ba.pitch, ba.yaw, ba.roll);
    }
    std::fprintf(f, "gameplay %s\n", gameplay::status());
#if defined(LIBFPV_KAMIKAZE)
    if (fpvapi::loaded()) std::fprintf(f, "kamikaze installed %d\n", fpv.fpv_kamikaze_is_installed());
#endif
    if (const fpv_frame *fr = drone::frame()) {
        std::fprintf(f, "cargo flags %u at %.1f %.1f %.1f vel %.1f %.1f %.1f\n", fr->cargo_flags, fr->cargo_position[0],
                     fr->cargo_position[1], fr->cargo_position[2], fr->cargo_velocity[0], fr->cargo_velocity[1],
                     fr->cargo_velocity[2]);
    }
    {
        sdk::Vector sp = pilot::spot();
        sdk::Vector vo = sdk::get<sdk::Vector>(p, "m_vecViewOffset");
        std::fprintf(f, "pilot standing_in %d spot %.0f %.0f %.0f eye %.0f %.0f %.0f\n", int(pilot::standing_in()), sp.x, sp.y,
                     sp.z, feet.x + vo.x, feet.y + vo.y, feet.z + vo.z);
        if (void *c = sdk::client_entity(1)) {
            int off = sdk::client_field_offset(c, "m_vecViewOffset");
            sdk::Vector cvo = off >= 0 ? *reinterpret_cast<sdk::Vector *>(static_cast<char *>(c) + off) : sdk::Vector{};
            std::fprintf(f, "client view offset %.1f %.1f %.1f (server %.1f %.1f %.1f)\n", cvo.x, cvo.y, cvo.z, vo.x, vo.y,
                         vo.z);
        }
    }
    for (void *door : sdk::find_entities("prop_testchamber_door")) {
        // the triggers around a door (what opens it), with their state
        sdk::Vector d = sdk::abs_origin(door);
        for (void *e : sdk::entities()) {
            const char *c = sdk::classname(e);
            if (std::strncmp(c, "trigger_", 8) != 0) continue;
            sdk::Vector lo, hi;
            if (!sdk::world_bounds(e, lo, hi)) continue;
            sdk::Vector mid = (lo + hi) * 0.5f;
            if ((mid - d).length() > 400) continue;
            std::fprintf(f, "  near %s: %s '%s' disabled %d %.0f %.0f %.0f .. %.0f %.0f %.0f\n", sdk::targetname(door), c,
                         sdk::targetname(e), int(sdk::get<bool>(e, "m_bDisabled", false)), lo.x, lo.y, lo.z, hi.x, hi.y,
                         hi.z);
        }
    }
    for (const char *cls : {"prop_weighted_cube", "npc_portal_turret_floor", "prop_testchamber_door", "prop_floor_button"})
        for (void *e : sdk::find_entities(cls)) {
            sdk::Vector o = sdk::abs_origin(e);
            std::fprintf(f, "%s '%s' %.0f %.0f %.0f%s\n", cls, sdk::targetname(e), o.x, o.y, o.z,
                         std::strcmp(cls, "prop_floor_button") == 0 && sdk::get<bool>(e, "m_bButtonState") ? " pressed" : "");
        }
    for (void *e : sdk::entities()) {
        if (std::strcmp(sdk::classname(e), "projected_tractor_beam_entity") == 0) {
            sdk::Vector a = sdk::get<sdk::Vector>(e, "m_vecStartPoint"), b = sdk::get<sdk::Vector>(e, "m_vecEndPoint");
            std::fprintf(f, "funnel %.0f %.0f %.0f -> %.0f %.0f %.0f\n", a.x, a.y, a.z, b.x, b.y, b.z);
            continue;
        }
        if (std::strcmp(sdk::classname(e), "prop_tractor_beam") == 0) {
            sdk::Vector o = sdk::abs_origin(e);
            std::fprintf(f, "funnel projector %.0f %.0f %.0f enabled %d force %.0f\n", o.x, o.y, o.z,
                         int(sdk::get<bool>(e, "m_bEnabled", false)), sdk::get<float>(e, "m_flLinearForce", 0));
            continue;
        }
        if (std::strcmp(sdk::classname(e), "env_portal_laser") != 0) continue;
        sdk::Vector a = sdk::get<sdk::Vector>(e, "m_vStartPoint"), b = sdk::get<sdk::Vector>(e, "m_vEndPoint");
        std::fprintf(f, "laser '%s' on %d %.0f %.0f %.0f -> %.0f %.0f %.0f\n", sdk::targetname(e),
                     int(sdk::get<bool>(e, "m_bLaserOn", false)), a.x, a.y, a.z, b.x, b.y, b.z);
    }
    std::fclose(f);
    MoveFileExA((path + ".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void dumps() {
    std::string path = paths::data_dir() + "\\dump.txt", text;
    if (!read_file(path, text)) return;
    DeleteFileA(path.c_str());
    std::istringstream in(text);
    std::string cls;
    while (in >> cls) {
        void *e = nullptr;
        if (cls == "player") e = sdk::player();
        else if (cls == "weapon") e = sdk::entity_from_handle(sdk::get<uint32_t>(sdk::player(), "m_hActiveWeapon", 0xFFFFFFFFu));
        else e = sdk::find_entity(cls.c_str());
        std::string out = e ? sdk::dump_fields(e) : "no " + cls;
        if (FILE *f = std::fopen((paths::data_dir() + "\\dump-" + cls + ".txt").c_str(), "w")) {
            for (char &c : out)
                if (c == ' ') c = '\n';
            std::fputs(out.c_str(), f);
            std::fclose(f);
        }
        fpvlog::write("devtools: dumped %s (%s)", cls.c_str(), e ? "found" : "none");
    }
}

// --- autotest -------------------------------------------------------------------------------

struct Step {
    float secs = 1;
    float ch[FPV_CHANNEL_COUNT];
    bool launch = false;
    std::vector<std::string> actions;  // run once when the step starts
};
std::vector<Step> g_steps;
size_t g_step = 0;
float g_step_time = 0;
bool g_running = false;

void parse_autotest(const std::string &text) {
    g_steps.clear();
    std::istringstream lines(text);
    std::string line;
    Step cur;
    for (int i = 0; i < FPV_CHANNEL_COUNT; i++) cur.ch[i] = (i == 2 || i >= 4) ? -1.0f : 0.0f;
    while (std::getline(lines, line)) {
        if (line.empty() || line[0] == '#') continue;
        Step s = cur;
        s.launch = false;
        s.actions.clear();
        std::istringstream words(line);
        std::string w;
        while (words >> w) {
            if (w == "launch") { s.launch = true; continue; }
            if (w.find('=') == std::string::npos || w.compare(0, 5, "view=") == 0 || w.compare(0, 5, "push=") == 0 || w.compare(0, 3, "at=") == 0 || w.compare(0, 5, "bomb=") == 0) { s.actions.push_back(w); continue; }
            size_t eq = w.find('=');
            if (eq == std::string::npos) continue;
            std::string k = w.substr(0, eq);
            float v = std::strtof(w.c_str() + eq + 1, nullptr);
            if (k == "secs") s.secs = v;
            else if (k == "roll") s.ch[0] = v;
            else if (k == "pitch") s.ch[1] = v;
            else if (k == "throttle") s.ch[2] = v;
            else if (k == "yaw") s.ch[3] = v;
            else if (k == "arm") s.ch[4] = v > 0 ? 1.0f : -1.0f;
            else if (k == "angle") s.ch[5] = v > 0 ? 1.0f : -1.0f;
        }
        cur = s;
        g_steps.push_back(s);
    }
}

// The autotest's words: respawn, at=x,y,z[,yaw], stop, f8, blue, orange, use, grab, drop,
// view=fpv|chase|los|pilot, shot (the mod's own screenshot), any console
// command after "cmd:" (no spaces: use '+' for one).
void run_action(const std::string &a) {
    fpvlog::write("autotest: %s", a.c_str());
    if (a == "respawn") drone::respawn();
    else if (a == "stop") drone::stop();
    else if (a == "blue") gameplay::press(gameplay::Action::FireBlue);
    else if (a == "orange") gameplay::press(gameplay::Action::FireOrange);
    else if (a == "use") gameplay::press(gameplay::Action::Use);
    else if (a == "grab") gameplay::press(gameplay::Action::GrabOn);
    else if (a == "drop") gameplay::press(gameplay::Action::GrabOff);
    else if (a == "f8") fpv.fpv_input_key(FPV_KEY_F8, 1), fpv.fpv_input_key(FPV_KEY_F8, 0);  // the overlay
    else if (a == "view=fpv") drone::set_view(drone::View::Fpv, false);
    else if (a == "view=chase") drone::set_view(drone::View::Chase, false);
    else if (a == "view=los") drone::set_view(drone::View::Los, false);
    else if (a == "view=pilot") drone::set_view(drone::View::Fpv, true);
#if defined(LIBFPV_KAMIKAZE)
    else if (a == "bomb=1" || a == "bomb=0") fpv.fpv_kamikaze_set_installed(a[5] == '1');  // saved per quad!
    else if (a == "boom" && drone::handle()) fpv.fpv_kamikaze_detonate(drone::handle(), 1);
#endif
    else if (a.compare(0, 5, "push=") == 0) {  // push=vx,vy,vz (units/s, replaces the velocity)
        float v[3] = {};
        if (std::sscanf(a.c_str() + 5, "%f,%f,%f", &v[0], &v[1], &v[2]) == 3 && drone::handle()) {
            fpv.fpv_drone_push(drone::handle(), v, 1);
        }
    } else if (a.compare(0, 3, "at=") == 0) {  // at=x,y,z[,yaw]
        float p[3] = {}, yaw = 0;
        if (std::sscanf(a.c_str() + 3, "%f,%f,%f,%f", &p[0], &p[1], &p[2], &yaw) >= 3) drone::respawn_at(p, yaw);
    } else if (a.compare(0, 4, "cmd:") == 0) {
        std::string c = a.substr(4);
        for (char &ch : c) if (ch == '+') ch = ' ';
        sdk::exec(c.c_str());
    }
}

}  // namespace

void tick() {
    write_status();
    dumps();
}

void frame(float dt) {
    std::string path = paths::data_dir() + "\\autotest.txt";
    bool exists = GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (!fpvapi::loaded() && !(exists && drone::load_libfpv())) return;
    if (!g_running && exists) {
        std::string text;
        if (!read_file(path, text)) return;
        parse_autotest(text);
        g_step = 0;
        g_step_time = 0;
        g_running = !g_steps.empty();
        fpvlog::write("autotest: %u steps", unsigned(g_steps.size()));
    }
    if (!g_running) return;
    if (!exists || g_step >= g_steps.size()) {
        g_running = false;
        DeleteFileA(path.c_str());
        fpv.fpv_input_clear_channels();
        fpvlog::write("autotest: done");
        return;
    }
    Step &s = g_steps[g_step];
    if (g_step_time == 0 && s.launch && !drone::handle()) drone::start();
    if (g_step_time == 0) for (const std::string &a : s.actions) run_action(a);
    fpv.fpv_input_set_channels(s.ch, FPV_CHANNEL_COUNT);
    g_step_time += dt;
    if (g_step_time >= s.secs) {
        g_step++;
        g_step_time = 0;
    }
}

}  // namespace devtools
