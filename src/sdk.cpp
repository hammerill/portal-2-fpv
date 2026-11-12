#include "sdk.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

#include "log.h"


namespace sdk {

Interfaces I;

using guest::addr;
using guest::g;
using guest::h;
using guest::r32;

// --- vtable indices: Windows (MSVC) and Linux (GCC, which gives a virtual
// destructor two slots), from SourceAutoRecord ----------------------------------------
namespace {
#if P2FPV_RECOMP
// server: CBaseEntity
constexpr int kGetServerClass = 10, kGetDataDescMap = 12;
// client: C_BaseEntity
constexpr int kClientGetDataDescMap = 17, kGetPredDescMap = 20, kClientGetClientClass = 19;
constexpr int kGetRefEHandle = 3;  // IHandleEntity (after ~IHandleEntity's two)
// IServerTools
constexpr int kGetIServerEntity = 2, kSetKeyValue = 12, kCreateEntityByName = 15, kDispatchSpawn = 16;
// m_EntPtrArray from GetIServerEntity's code; the client mode from HudProcessInput's
constexpr int kEntPtrArrayAt = 45, kGetClientModeCall = 9, kClientModesAt = 20;
// IEngineTool (CEngineTool's vtable in engine.so: slot 61 is a drawing
// function there, 62 validates the path and precaches)
constexpr int kPrecacheModel = 62;
#else
constexpr int kPrecacheModel = 61;
constexpr int kGetServerClass = 9, kGetDataDescMap = 11;
constexpr int kClientGetDataDescMap = 15, kGetPredDescMap = 17, kClientGetClientClass = 18;
constexpr int kGetRefEHandle = 2;
constexpr int kGetIServerEntity = 1, kSetKeyValue = 13, kCreateEntityByName = 14, kDispatchSpawn = 15;
constexpr int kEntPtrArrayAt = 61, kGetClientModeCall = 4, kClientModesAt = 19;
#endif
// the same on both
constexpr int kFindCommandBase = 13, kExecuteClientCmd = 104, kIsPaused = 86, kGetScreenSize = 5, kGetViewAngles = 18,
              kSetViewAngles = 19, kHudProcessInput = 10, kTraceRay = 5, kGetClientEntity = 3,
              kGetGlobalVars = 1;
// IClientNetworkable (at +8 in C_BaseEntity): GetClientClass is its third function
constexpr int kNetworkableOffset = 8, kNetworkableGetClientClass = 2;
}  // namespace

// --- maths -------------------------------------------------------------------------------

namespace {
constexpr float kDeg = 3.14159265f / 180.0f;
}

void angle_vectors(const QAngle &a, Vector *forward, Vector *right, Vector *up) {
    float sp = std::sin(a.pitch * kDeg), cp = std::cos(a.pitch * kDeg);
    float sy = std::sin(a.yaw * kDeg), cy = std::cos(a.yaw * kDeg);
    float sr = std::sin(a.roll * kDeg), cr = std::cos(a.roll * kDeg);
    if (forward) *forward = {cp * cy, cp * sy, -sp};
    if (right) *right = {-sr * sp * cy + cr * sy, -sr * sp * sy - cr * cy, -sr * cp};
    if (up) *up = {cr * sp * cy + sr * sy, cr * sp * sy - sr * cy, cr * cp};
}

QAngle vectors_angles(const Vector &f, const Vector &up) {
    QAngle a;
    float horiz = std::sqrt(f.x * f.x + f.y * f.y);
    a.pitch = std::atan2(-f.z, horiz) / kDeg;
    if (horiz > 1e-4f) {
        a.yaw = std::atan2(f.y, f.x) / kDeg;
    } else {
        // straight up or down: the yaw that leaves no roll (up = the unrolled up)
        a.yaw = f.z < 0 ? std::atan2(up.y, up.x) / kDeg : std::atan2(-up.y, -up.x) / kDeg;
    }
    // roll: the up vector between the unrolled up and the unrolled right
    Vector u0, r0;
    QAngle flat = a;
    flat.roll = 0;
    angle_vectors(flat, nullptr, &r0, &u0);
    a.roll = std::atan2(up.dot(r0), up.dot(u0)) / kDeg;  // up = cos(roll) u0 + sin(roll) r0
    return a;
}

// --- vtables ------------------------------------------------------------------------------

bool VtableHook::install(void *object, int index, addr fn) {
    if (!object || slot || !fn) return false;
    addr s = r32(g(object)) + 4 * uint32_t(index);
    addr old = r32(s);
    if (!guest::patch(s, &fn, 4)) return false;
    original = old;
    slot = s;
    return true;
}

void VtableHook::restore() {
    if (!slot) return;
    guest::patch(slot, &original, 4);
    slot = 0;
}

// --- tables ------------------------------------------------------------------------------

namespace {

enum FieldType { FIELD_EMBEDDED = 10 };

// The engine's structures, pointers as game addresses.
struct typedescription_t {
    int fieldType;
    addr fieldName;
    int fieldOffset;
    unsigned short fieldSize;
    short flags;
    addr externalName;
    addr pSaveRestoreOps;
    // inputfunc_t, a pointer to member function: one word with MSVC, two with
    // GCC (Itanium ABI: the function and the this adjustment)
#if P2FPV_RECOMP
    addr inputFunc[2];
#else
    addr inputFunc;
#endif
    addr td;  // datamap_t*
    int fieldSizeInBytes;
    addr override_field;
    int override_count;
    float fieldTolerance;
    int flatOffset[2];
    unsigned short flatGroup;
};
static_assert(sizeof(typedescription_t) == (P2FPV_RECOMP ? 64 : 60), "typedescription_t (32-bit)");
struct datamap_t {
    addr dataDesc;  // typedescription_t*
    int dataNumFields;
    addr dataClassName;
    addr baseMap;  // datamap_t*
};

struct SendProp {
    addr vmt;
    addr matching;
    int type;  // 6 = DPT_DataTable
    int bits;
    float low, high;
    addr arrayProp;
    addr arrayLengthProxy;
    int elements, elementStride;
    addr excludeName, parentArrayName, varName;
    float highLowMul;
    char priority;
    int flags;
    addr proxy, dataTableProxy;
    addr dataTable;  // SendTable*
    int offset;
    addr extra;
};
struct SendTable {
    addr props;  // SendProp*
    int count;
    addr name;
};
struct ServerClass {
    addr name;
    addr table;  // SendTable*
    addr next;
    int id;
};

struct RecvProp {
    addr varName;
    int type, flags, stringBufferSize;
    bool insideArray;
    addr extra;
    addr arrayProp;
    addr arrayLengthProxy, proxy, dataTableProxy;
    addr dataTable;  // RecvTable*
    int offset, elementStride, elements;
    addr parentArrayName;
};
struct RecvTable {
    addr props;  // RecvProp*
    int count;
    addr decoder;
    addr name;
};
struct ClientClass {
    addr create, createEvent;
    addr name;
    addr table;  // RecvTable*
    addr next;
    int id;
};

using FieldMap = std::unordered_map<std::string, int>;

void add_field(FieldMap &m, const std::string &name, int offset) { m.emplace(name, offset); }

void walk_datamap(addr dma, int base, const std::string &prefix, FieldMap &out, int depth = 0) {
    for (; dma && depth < 32; depth++) {
        const auto *dm = guest::at<datamap_t>(dma);
        const auto *fields = guest::at<typedescription_t>(dm->dataDesc);
        for (int i = 0; fields && i < dm->dataNumFields; i++) {
            const typedescription_t &f = fields[i];
            if (!f.fieldName) continue;
            const char *name = guest::str_at(f.fieldName);
            int off = base + f.fieldOffset;
            if (f.fieldType == FIELD_EMBEDDED && f.td) {
                walk_datamap(f.td, off, prefix + name + ".", out, depth + 1);
                // embedded members are also reachable without the prefix (m_Collision.m_vecMins as m_vecMins)
                walk_datamap(f.td, off, "", out, depth + 1);
            }
            add_field(out, prefix + name, off);
        }
        dma = dm->baseMap;
    }
}

void walk_sendtable(addr ta, int base, FieldMap &out, int depth = 0) {
    if (!ta || depth > 16) return;
    const auto *t = guest::at<SendTable>(ta);
    const auto *props = guest::at<SendProp>(t->props);
    for (int i = 0; props && i < t->count; i++) {
        const SendProp &p = props[i];
        if (!p.varName) continue;
        if (p.type == 6 && p.dataTable) {
            walk_sendtable(p.dataTable, base + p.offset, out, depth + 1);
            continue;
        }
        add_field(out, guest::str_at(p.varName), base + p.offset);
    }
}

void walk_recvtable(addr ta, int base, FieldMap &out, int depth = 0) {
    if (!ta || depth > 16 || !guest::readable(ta, sizeof(RecvTable))) return;
    const auto *t = guest::at<RecvTable>(ta);
    if (t->count < 0 || t->count > 4096 || !guest::readable(t->props, sizeof(RecvProp) * t->count)) return;
    const auto *props = guest::at<RecvProp>(t->props);
    for (int i = 0; i < t->count; i++) {
        const RecvProp &p = props[i];
        if (!p.varName || !guest::readable(p.varName, 1)) continue;
        if (p.type == 6 && p.dataTable) {
            walk_recvtable(p.dataTable, base + p.offset, out, depth + 1);
            continue;
        }
        add_field(out, guest::str_at(p.varName), base + p.offset);
    }
}

std::unordered_map<addr, FieldMap> g_server_fields, g_client_fields;

const FieldMap &server_fields(void *ent) {
    addr dm = g(vcall<void *>(ent, kGetDataDescMap));
    auto it = g_server_fields.find(dm);
    if (it != g_server_fields.end()) return it->second;
    FieldMap &m = g_server_fields[dm];
    walk_datamap(dm, 0, "", m);  // datamaps first: they have every saved field
    if (void *sc = vcall<void *>(ent, kGetServerClass)) walk_sendtable(static_cast<ServerClass *>(sc)->table, 0, m);
    return m;
}

bool is_client_class(addr cc) {
    if (!cc || !guest::readable(cc, sizeof(ClientClass))) return false;
    const auto *c = guest::at<ClientClass>(cc);
    return c->name && guest::readable(c->name, 1) && c->table && guest::readable(c->table, sizeof(RecvTable));
}

addr client_class_of(void *ent) {
    void *net = static_cast<char *>(ent) + kNetworkableOffset;
    addr cc = g(vcall<void *>(net, kNetworkableGetClientClass));
    if (is_client_class(cc)) return cc;
    cc = g(vcall<void *>(ent, kClientGetClientClass));
    return is_client_class(cc) ? cc : 0;
}

const FieldMap &client_fields(void *ent) {
    addr dm = g(vcall<void *>(ent, kClientGetDataDescMap));
    auto it = g_client_fields.find(dm);
    if (it != g_client_fields.end()) return it->second;
    FieldMap &m = g_client_fields[dm];
    walk_datamap(dm, 0, "", m);
    walk_datamap(g(vcall<void *>(ent, kGetPredDescMap)), 0, "", m);
    if (addr cc = client_class_of(ent)) walk_recvtable(guest::at<ClientClass>(cc)->table, 0, m);
    else fpvlog::write("sdk: no client class for %08x", g(ent));
    return m;
}

struct CEntInfo {
    addr entity;
    int serial;
    addr prev, next;
    addr name, classname;
};
constexpr int kEntEntries = 0x2000;

CEntInfo *ent_infos() { return static_cast<CEntInfo *>(I.ent_infos); }

template <typename T>
T read(addr a) {
    T v{};
    if (guest::readable(a, sizeof(T))) std::memcpy(&v, h(a), sizeof(T));
    return v;
}

void *factory_call(addr factory, const char *name) {
    if (!factory) return nullptr;
    guest::Scratch scratch;
    return guest::call<void *>(factory, guest::str(name), static_cast<int *>(nullptr));
}

void *module_factory(const char *module, const char *name) {
    return factory_call(guest::module_symbol(module, "CreateInterface"), name);
}

// The CEntInfo array at `a`, if CGlobalEntityList's vtable is right before it.
bool is_entity_list(addr a) {
    addr server = 0;
    size_t size = 0;
    if (!a || !guest::module_range("server", server, size)) return false;
    if (!guest::readable(a - 4, sizeof(CEntInfo) * kEntEntries + 4)) return false;
    addr vt = r32(a - 4);
    return vt >= server && vt < server + size;
}

// The trace filter the engine calls: an ITraceFilter in game memory whose
// vtable points at the functions below, and the entities to pass through.
struct GameFilter {
    addr vtable;
    int count;
    addr skip[8];
};
addr g_filter_vtable = 0;

bool should_hit(const GameFilter *f, addr ent) {
    for (int i = 0; i < f->count && i < 8; i++)
        if (f->skip[i] == ent) return false;
    return true;
}

#if P2FPV_RECOMP
void filter_should_hit(void *cpu, addr args, void *) {
    // (this, IHandleEntity *, int contentsMask)
    guest::host->set_return(cpu, should_hit(guest::at<GameFilter>(r32(args)), r32(args + 4)) ? 1 : 0);
}
void filter_trace_type(void *cpu, addr, void *) { guest::host->set_return(cpu, 0); }  // TRACE_EVERYTHING
#else
bool __thiscall filter_should_hit(const GameFilter *self, void *ent, int) { return should_hit(self, g(ent)); }
int __thiscall filter_trace_type(const GameFilter *) { return 0; }
#endif

}  // namespace

void install_callbacks() {
    if (!g_filter_vtable) {
#if P2FPV_RECOMP
        g_filter_vtable = guest::host->alloc(16);
#else
        static addr table[2];
        g_filter_vtable = g(table);
#endif
        if (!g_filter_vtable) return;
    }
    addr *vt = guest::at<addr>(g_filter_vtable);
#if P2FPV_RECOMP
    vt[0] = guest::host->define("p2fpv.TraceFilter.ShouldHitEntity", &filter_should_hit, nullptr);
    vt[1] = guest::host->define("p2fpv.TraceFilter.GetTraceType", &filter_trace_type, nullptr);
#else
    vt[0] = g(reinterpret_cast<void *>(&filter_should_hit));
    vt[1] = g(reinterpret_cast<void *>(&filter_trace_type));
#endif
}

// --- interfaces ------------------------------------------------------------------------------

bool init(addr engine_factory, addr server_factory) {
    install_callbacks();
    auto eng = [&](const char *n) { return engine_factory ? factory_call(engine_factory, n) : module_factory("engine", n); };
    auto srv = [&](const char *n) { return server_factory ? factory_call(server_factory, n) : module_factory("server", n); };
    I.engine_client = eng("VEngineClient015");
    I.engine_server = eng("VEngineServer022");
    I.engine_tool = eng("VENGINETOOL003");
    I.trace = eng("EngineTraceServer004");
    I.cvar = eng("VEngineCvar007");
    if (!I.cvar) I.cvar = module_factory("vstdlib", "VEngineCvar007");
    I.client_dll = module_factory("client", "VClient016");
    I.client_entities = module_factory("client", "VClientEntityList003");
    I.server_tools = srv("VSERVERTOOLS001");
    I.server_game_dll = srv("ServerGameDLL005");
    I.player_info = srv("PlayerInfoManager002");
    if (I.player_info) I.globals = vcall<CGlobalVars *>(I.player_info, kGetGlobalVars);

    // the server's entity list, read from IServerTools::GetIServerEntity's code
    if (I.server_tools && !I.ent_infos) {
        addr fn = guest::vfunc(I.server_tools, kGetIServerEntity);
        addr at = read<addr>(fn + kEntPtrArrayAt);
        // the address read is CGlobalEntityList itself on the PIC Linux build
        // (SourceAutoRecord's EIPRelPIC), its array 4 bytes in
        for (addr a : {at, at + 4})
            if (is_entity_list(a)) {
                I.ent_infos = h(a);
                break;
            }
        if (!I.ent_infos) fpvlog::write("sdk: no entity list near %08x (GetIServerEntity %08x)", at, fn);
    }
    // the client mode, from IBaseClientDLL::HudProcessInput: call GetClientMode, which returns g_pClientMode[0]
    if (I.client_dll && !I.client_mode) {
        addr hud = guest::vfunc(I.client_dll, kHudProcessInput);
        addr call = hud + kGetClientModeCall;  // the rel32 of a call
        if (read<uint8_t>(call - 1) == 0xE8) {
            addr get_client_mode = call + read<int>(call) + 4;
            addr modes = read<addr>(get_client_mode + kClientModesAt);
            addr mode = read<addr>(modes);
            if (mode && guest::readable(read<addr>(mode), 64 * 4)) I.client_mode = h(mode);
        }
        if (!I.client_mode) {
            const uint8_t *b = static_cast<const uint8_t *>(h(hud));
            fpvlog::write("sdk: no client mode (HudProcessInput %08x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
                          "%02x %02x %02x %02x %02x)",
                          hud, b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
                          b[15]);
        }
    }
    fpvlog::write("sdk: engine client %08x server %08x tool %08x trace %08x cvar %08x client %08x entities %08x tools %08x "
                  "game %08x globals %08x entity list %08x client mode %08x",
                  g(I.engine_client), g(I.engine_server), g(I.engine_tool), g(I.trace), g(I.cvar), g(I.client_dll),
                  g(I.client_entities), g(I.server_tools), g(I.server_game_dll), g(I.globals), g(I.ent_infos),
                  g(I.client_mode));
    return I.engine_client && I.trace && I.cvar && I.client_dll && I.client_entities && I.globals && I.ent_infos &&
           I.client_mode;
}

// --- engine helpers ------------------------------------------------------------------------

void exec(const char *cmd) {
    if (!I.engine_client) return;
    guest::Scratch scratch;
    vcall<void>(I.engine_client, kExecuteClientCmd, guest::str(cmd));
}

void execf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    exec(buf);
}

namespace {
char *find_command(const char *name) {
    if (!I.cvar) return nullptr;
    guest::Scratch scratch;
    return vcall<char *>(I.cvar, kFindCommandBase, guest::str(name));
}
}  // namespace

bool uncheat(const char *name) {
    char *base = find_command(name);
    if (!base) return false;
    *reinterpret_cast<int *>(base + 20) &= ~(1 << 14);  // ConCommandBase::m_nFlags, FCVAR_CHEAT
    return true;
}

std::string cvar_string(const char *name) {
    char *base = find_command(name);
    if (!base) return "";
    addr parent = r32(g(base) + 28);  // ConVar::m_pParent
    if (!parent) parent = g(base);
    addr s = r32(parent + 36);  // m_pszString
    return s ? guest::str_at(s) : "";
}

bool is_paused() { return I.engine_client && vcall<bool>(I.engine_client, kIsPaused); }

bool in_game() { return I.globals && player() != nullptr; }

void screen_size(int &w, int &h) {
    w = 1280, h = 720;
    if (!I.engine_client) return;
    guest::Scratch scratch;
    int *pw = guest::temp<int>(w), *ph = guest::temp<int>(h);
    vcall<void>(I.engine_client, kGetScreenSize, pw, ph);
    w = *pw, h = *ph;
}

QAngle view_angles() {
    if (!I.engine_client) return {};
    guest::Scratch scratch;
    QAngle *a = guest::temp<QAngle>();
    vcall<void>(I.engine_client, kGetViewAngles, a);
    return *a;
}

void set_view_angles(const QAngle &a) {
    if (!I.engine_client) return;
    guest::Scratch scratch;
    vcall<void>(I.engine_client, kSetViewAngles, guest::temp(a));
}

// --- server entities ----------------------------------------------------------------------

std::vector<void *> entities() {
    std::vector<void *> out;
    CEntInfo *infos = ent_infos();
    if (!infos) return out;
    for (int i = 0; i < kEntEntries; i++)
        if (infos[i].entity) out.push_back(h(infos[i].entity));
    return out;
}

const char *classname(void *ent) {
    addr s = get<addr>(ent, "m_iClassname", 0);
    return s ? guest::str_at(s) : "";
}

const char *targetname(void *ent) {
    addr s = get<addr>(ent, "m_iName", 0);
    return s ? guest::str_at(s) : "";
}

void *find_entity(const char *cls) {
    for (void *e : entities())
        if (std::strcmp(classname(e), cls) == 0) return e;
    return nullptr;
}

std::vector<void *> find_entities(const char *cls) {
    std::vector<void *> out;
    for (void *e : entities())
        if (std::strcmp(classname(e), cls) == 0) out.push_back(e);
    return out;
}

uint32_t handle_of(void *ent) {
    if (!ent) return 0xFFFFFFFFu;
    const uint32_t *hd = vcall<const uint32_t *>(ent, kGetRefEHandle);
    return hd ? *hd : 0xFFFFFFFFu;
}

int entity_index(void *ent) { return ent ? int(handle_of(ent) & 0xFFFF) : -1; }

void *entity_from_handle(uint32_t hd) {
    CEntInfo *infos = ent_infos();
    if (hd == 0xFFFFFFFFu || !infos) return nullptr;
    int idx = int(hd & 0xFFFF);
    if (idx >= kEntEntries) return nullptr;
    const CEntInfo &info = infos[idx];
    return info.entity && uint32_t(info.serial) == (hd >> 16) ? h(info.entity) : nullptr;
}

void *player() {
    CEntInfo *infos = ent_infos();
    return infos ? h(infos[1].entity) : nullptr;
}

int field_offset(void *ent, const char *name) {
    if (!ent) return -1;
    const FieldMap &m = server_fields(ent);
    auto it = m.find(name);
    return it == m.end() ? -1 : it->second;
}

Vector abs_origin(void *ent) { return get<Vector>(ent, "m_vecAbsOrigin"); }
QAngle abs_angles(void *ent) { return get<QAngle>(ent, "m_angAbsRotation"); }

bool world_bounds(void *ent, Vector &mins, Vector &maxs) {
    Vector *lo = field<Vector>(ent, "m_Collision.m_vecMins"), *hi = field<Vector>(ent, "m_Collision.m_vecMaxs");
    if (!lo || !hi) return false;
    Vector o = abs_origin(ent), f, r, u;
    angle_vectors(abs_angles(ent), &f, &r, &u);
    // the local box (x forward, y left, z up) turned into the world, then its AABB
    Vector left = -r;
    mins = maxs = o + f * lo->x + left * lo->y + u * lo->z;
    for (int i = 1; i < 8; i++) {
        Vector c = o + f * (i & 1 ? hi->x : lo->x) + left * (i & 2 ? hi->y : lo->y) + u * (i & 4 ? hi->z : lo->z);
        mins = {std::min(mins.x, c.x), std::min(mins.y, c.y), std::min(mins.z, c.z)};
        maxs = {std::max(maxs.x, c.x), std::max(maxs.y, c.y), std::max(maxs.z, c.z)};
    }
    return true;
}

std::string dump_fields(void *ent) {
    if (!ent) return "";
    const FieldMap &m = server_fields(ent);
    std::vector<std::pair<int, std::string>> v;
    for (auto &kv : m) v.push_back({kv.second, kv.first});
    std::sort(v.begin(), v.end());
    std::string out = std::string(classname(ent)) + ":";
    for (auto &p : v) out += " " + p.second + "@" + std::to_string(p.first);
    return out;
}

void *entity_by_index(int index) {
    CEntInfo *infos = ent_infos();
    if (!infos || index < 0 || index >= kEntEntries) return nullptr;
    return h(infos[index].entity);
}

void *create_entity(const char *cls) {
    if (!I.server_tools) return nullptr;
    guest::Scratch scratch;
    return vcall<void *>(I.server_tools, kCreateEntityByName, guest::str(cls));
}

bool set_keyvalue(void *ent, const char *key, const char *value) {
    if (!I.server_tools || !ent) return false;
    guest::Scratch scratch;
    return vcall<bool>(I.server_tools, kSetKeyValue, ent, guest::str(key), guest::str(value));
}

void dispatch_spawn(void *ent) {
    if (I.server_tools && ent) vcall<void>(I.server_tools, kDispatchSpawn, ent);
}

bool readable_str(const char *s) { return s && guest::readable(g(s), 1); }

int precache_model(const char *path) {
    if (!I.engine_tool) return 0;
    guest::Scratch scratch;
    return vcall<int>(I.engine_tool, kPrecacheModel, guest::str(path), true);
}

addr scan(const char *module, const char *pattern) {
    addr base = 0;
    size_t size = 0;
    if (!guest::module_range(module, base, size)) return 0;
    const uint8_t *bytes_at = static_cast<const uint8_t *>(h(base));
    std::vector<int> bytes;  // -1 = any
    for (const char *p = pattern; *p;) {
        if (*p == ' ') { p++; continue; }
        if (*p == '?') { bytes.push_back(-1); while (*p == '?') p++; continue; }
        bytes.push_back(int(std::strtol(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    if (bytes.empty() || size < bytes.size()) return 0;
    for (size_t i = 0; i + bytes.size() <= size; i++) {
        size_t j = 0;
        while (j < bytes.size() && (bytes[j] < 0 || bytes_at[i + j] == bytes[j])) j++;
        if (j == bytes.size()) return base + addr(i);
    }
    return 0;
}

void *client_entity(int index) {
    return I.client_entities ? vcall<void *>(I.client_entities, kGetClientEntity, index) : nullptr;
}

int client_field_offset(void *ent, const char *name) {
    if (!ent) return -1;
    const FieldMap &m = client_fields(ent);
    auto it = m.find(name);
    return it == m.end() ? -1 : it->second;
}

// --- traces ------------------------------------------------------------------------------

namespace {
// The engine's CGameTrace.
struct RawTrace {
    Vector startpos, endpos;
    struct {
        Vector normal;
        float dist;
        unsigned char type, signbits, pad[2];
    } plane;
    float fraction;
    int contents;
    unsigned short dispFlags;
    bool allsolid, startsolid;
    float fractionleftsolid;
    struct {
        addr name;
        short surfaceProps;
        unsigned short flags;
    } surface;
    int hitgroup;
    short physicsbone;
    unsigned short worldSurfaceIndex;
    addr ent;
    int hitbox;
};
static_assert(sizeof(RawTrace) == 84, "CGameTrace (32-bit)");
}  // namespace

CGameTrace read_trace(const void *raw) {
    const auto &r = *static_cast<const RawTrace *>(raw);
    CGameTrace t;
    t.startpos = r.startpos, t.endpos = r.endpos;
    t.plane.normal = r.plane.normal, t.plane.dist = r.plane.dist;
    t.fraction = r.fraction;
    t.contents = r.contents;
    t.allsolid = r.allsolid, t.startsolid = r.startsolid;
    t.surface.name = r.surface.name && guest::readable(r.surface.name, 1) ? guest::str_at(r.surface.name) : nullptr;
    t.ent = h(r.ent);
    return t;
}

void trace_ray(const Vector &from, const Vector &to, uint32_t mask, TraceFilter *filter, CGameTrace &out) {
    out = CGameTrace{};
    if (!I.trace || !g_filter_vtable) return;
#if P2FPV_RECOMP
    if (!guest::cpu()) return;  // not on a game thread: nothing hit
#endif
    guest::Scratch scratch;
    Ray *ray = guest::temp(Ray(from, to));
    auto *f = guest::temp<GameFilter>();
    f->vtable = g_filter_vtable;
    for (int i = 0; filter && i < filter->skip_count; i++) f->skip[f->count++] = g(filter->skip[i]);
    auto *raw = guest::temp<RawTrace>();
    vcall<void>(I.trace, kTraceRay, static_cast<const Ray *>(ray), mask, static_cast<void *>(f), static_cast<void *>(raw));
    out = read_trace(raw);
}

}  // namespace sdk
