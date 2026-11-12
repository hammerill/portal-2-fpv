// A minimal Source engine SDK for Portal 2 (build 2.0.0.1, 32-bit), written
// for this mod: no Valve SDK headers. Vtable indices and struct layouts are
// those SourceAutoRecord uses for the current Portal 2 build
// (https://github.com/p2sr/SourceAutoRecord, MIT), for Windows and for Linux
// (the build source-recomp runs on macOS); entity fields are looked up by
// name at runtime in the game's own datamaps and network tables, so they
// follow the game's layout. Pointers inside engine structures are 32-bit
// game addresses (guest.h).
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "guest.h"

namespace sdk {

// --- maths -----------------------------------------------------------------------------

struct Vector {
    float x = 0, y = 0, z = 0;
    Vector() = default;
    Vector(float a, float b, float c) : x(a), y(b), z(c) {}
    explicit Vector(const float v[3]) : x(v[0]), y(v[1]), z(v[2]) {}
    Vector operator+(const Vector &o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vector operator-(const Vector &o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vector operator*(float k) const { return {x * k, y * k, z * k}; }
    Vector operator-() const { return {-x, -y, -z}; }
    Vector &operator+=(const Vector &o) { x += o.x, y += o.y, z += o.z; return *this; }
    float dot(const Vector &o) const { return x * o.x + y * o.y + z * o.z; }
    Vector cross(const Vector &o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    float length() const { return std::sqrt(dot(*this)); }
    Vector normalized() const {
        float l = length();
        return l > 1e-6f ? *this * (1.0f / l) : Vector{};
    }
    void to(float out[3]) const { out[0] = x, out[1] = y, out[2] = z; }
};

// pitch (down positive), yaw, roll in degrees, Source's order
struct QAngle {
    float pitch = 0, yaw = 0, roll = 0;
};

struct alignas(16) VectorAligned {
    float x = 0, y = 0, z = 0, w = 0;
    VectorAligned() = default;
    VectorAligned(const Vector &v) : x(v.x), y(v.y), z(v.z), w(0) {}
};

// Forward, right and up of Source angles (mathlib AngleVectors).
void angle_vectors(const QAngle &a, Vector *forward, Vector *right, Vector *up);
// Angles of an orientation given its forward and up (any roll, also straight
// up or down).
QAngle vectors_angles(const Vector &forward, const Vector &up);

// --- vtables ---------------------------------------------------------------------------

using guest::vcall;

// Swaps one slot of a vtable (shared by every object of the class) for a
// function the game can call (guest.h); restore() puts the original back.
// Main thread only.
struct VtableHook {
    guest::addr slot = 0;
    guest::addr original = 0;
    bool install(void *object, int index, guest::addr fn);
    void restore();
};

// --- engine structures -------------------------------------------------------------------

struct CViewSetup {
    int x, m_nUnscaledX, y, m_nUnscaledY, width, m_nUnscaledWidth, height, m_nUnscaledHeight;
    bool m_bOrtho;
    float m_OrthoLeft, m_OrthoTop, m_OrthoRight, m_OrthoBottom;
    bool m_bCustomViewMatrix;
    float m_matCustomViewMatrix[3][4];
    float fov;  // horizontal, for a 4:3 screen (the engine widens it for the real aspect)
    float fovViewmodel;
    Vector origin;
    QAngle angles;
    float zNear, zFar, zNearViewmodel, zFarViewmodel;
    float m_flAspectRatio;
};

struct CUserCmd {
    guest::addr vmt;
    int command_number, tick_count;
    QAngle viewangles;
    float forwardmove, sidemove, upmove;
    int buttons;
    unsigned char impulse;
    int weaponselect, weaponsubtype, random_seed;
    short mousedx, mousedy;
    bool hasbeenpredicted;
};

enum : int {
    IN_ATTACK = 1 << 0, IN_JUMP = 1 << 1, IN_DUCK = 1 << 2, IN_FORWARD = 1 << 3, IN_BACK = 1 << 4,
    IN_USE = 1 << 5, IN_ATTACK2 = 1 << 11,
};

enum : uint32_t {
    CONTENTS_SOLID = 0x1, CONTENTS_WINDOW = 0x2, CONTENTS_GRATE = 0x8, CONTENTS_SLIME = 0x10, CONTENTS_WATER = 0x20,
    CONTENTS_MOVEABLE = 0x4000, CONTENTS_PLAYERCLIP = 0x10000, CONTENTS_MONSTER = 0x2000000, CONTENTS_DEBRIS = 0x4000000,
    MASK_SOLID = CONTENTS_SOLID | CONTENTS_MOVEABLE | CONTENTS_WINDOW | CONTENTS_MONSTER | CONTENTS_GRATE,
    SURF_SKY = 0x4, SURF_NOPORTAL = 0x20,
};

struct Ray {
    VectorAligned start, delta, start_offset, extents;
    guest::addr world_axis_transform = 0;
    bool is_ray = true, is_swept = true;
    Ray(const Vector &from, const Vector &to) : start(from), delta(to - from) {
        is_swept = (to - from).length() > 0;
    }
};

// A trace's result, read out of the engine's (CGameTrace) into mod pointers.
struct CGameTrace {
    Vector startpos, endpos;
    struct {
        Vector normal;
        float dist;
    } plane;
    float fraction = 1;
    int contents = 0;
    bool allsolid = false, startsolid = false;
    struct {
        const char *name = nullptr;  // nullptr if unreadable
    } surface;
    void *ent = nullptr;  // CBaseEntity* (server)
};
// A CGameTrace in the engine's layout (`raw`, game memory), read out.
CGameTrace read_trace(const void *raw);

// What a trace passes through (the engine's ITraceFilter is made from it).
struct TraceFilter {
    void *skip[8] = {};
    int skip_count = 0;
    void add(void *ent) {
        if (ent && skip_count < 8) skip[skip_count++] = ent;
    }
};

// --- interfaces ----------------------------------------------------------------------------

struct CGlobalVars {
    float realtime;
    int framecount;
    float absoluteframetime, curtime, frametime;
    int maxClients, tickcount;
    float interval_per_tick;
};

struct Interfaces {
    void *engine_client = nullptr;     // VEngineClient015
    void *engine_server = nullptr;     // VEngineServer022
    void *engine_tool = nullptr;       // VENGINETOOL003
    void *trace = nullptr;             // EngineTraceServer004
    void *cvar = nullptr;              // VEngineCvar007
    void *client_dll = nullptr;        // VClient016
    void *client_entities = nullptr;   // VClientEntityList003
    void *server_tools = nullptr;      // VSERVERTOOLS001
    void *server_game_dll = nullptr;   // ServerGameDLL005
    void *player_info = nullptr;       // PlayerInfoManager002
    void *client_mode = nullptr;       // ClientModePortalNormal, from IBaseClientDLL::HudProcessInput
    CGlobalVars *globals = nullptr;
    void *ent_infos = nullptr;         // CEntInfo m_EntPtrArray[], the server's entity list
};

extern Interfaces I;

// Resolves everything above; logs and returns false on what is missing.
// The factories are IServerPluginCallbacks::Load's, game functions.
bool init(guest::addr engine_factory, guest::addr server_factory);
// The vtable functions the engine calls on the mod's trace filters (macOS:
// defined anew by each core).
void install_callbacks();

// --- engine helpers -------------------------------------------------------------------------

void exec(const char *cmd);  // runs a console command now (ExecuteClientCmd)
void execf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Clears FCVAR_CHEAT on a console command or variable, so the mod can use it
// without sv_cheats. Returns false if the command does not exist.
bool uncheat(const char *name);
// A console variable's value as a string ("" if missing).
std::string cvar_string(const char *name);

bool is_paused();
bool in_game();  // a map is loaded and the local player exists
void screen_size(int &w, int &h);
QAngle view_angles();
void set_view_angles(const QAngle &a);

// --- server entities ----------------------------------------------------------------------

// Every server entity (CBaseEntity*), in index order.
std::vector<void *> entities();
// The first / every entity with this class name.
void *find_entity(const char *classname);
std::vector<void *> find_entities(const char *classname);
// The entity of a handle (EHANDLE), or nullptr.
void *entity_from_handle(uint32_t handle);
uint32_t handle_of(void *ent);
void *player();  // the local player (index 1)

// Field offset in a server entity by name (datamap first, then send tables),
// -1 if it has none. Cached per class.
int field_offset(void *ent, const char *name);
template <typename T>
T *field(void *ent, const char *name) {
    int off = ent ? field_offset(ent, name) : -1;
    return off >= 0 ? reinterpret_cast<T *>(static_cast<char *>(ent) + off) : nullptr;
}
template <typename T>
T get(void *ent, const char *name, T fallback = T{}) {
    T *p = field<T>(ent, name);
    return p ? *p : fallback;
}
const char *classname(void *ent);
const char *targetname(void *ent);
int entity_index(void *ent);
Vector abs_origin(void *ent);
QAngle abs_angles(void *ent);
// World-space bounds of the entity's collision box (the OBB's AABB).
bool world_bounds(void *ent, Vector &mins, Vector &maxs);
// Every field of the entity's class, for working out new hooks.
std::string dump_fields(void *ent);

void *entity_by_index(int index);
// IServerTools: a new entity, its keyvalues, then spawned.
void *create_entity(const char *classname);
bool set_keyvalue(void *ent, const char *key, const char *value);
void dispatch_spawn(void *ent);
// A NUL-terminated string can be read at `s` (its first 64 bytes, at least).
bool readable_str(const char *s);

// IEngineTool::PrecacheModel (61, SAR); late precaching works in single player.
int precache_model(const char *path);

// A code pattern ("55 8B EC ?? ...") in a loaded module ("server": server.dll
// or server.so), or 0.
guest::addr scan(const char *module, const char *pattern);

// Same for client entities (C_BaseEntity* from IClientEntityList).
void *client_entity(int index);
int client_field_offset(void *ent, const char *name);

// Server-side engine trace (IEngineTrace::TraceRay).
void trace_ray(const Vector &from, const Vector &to, uint32_t mask, TraceFilter *filter, CGameTrace &out);

}  // namespace sdk
