#include "sdk.h"

#include <windows.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <unordered_map>

#include "log.h"

namespace sdk {

Interfaces I;

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

bool VtableHook::install(void *object, int index, void *fn) {
    if (!object || slot) return false;
    void **vt = *reinterpret_cast<void ***>(object);
    void **s = &vt[index];
    DWORD old;
    if (!VirtualProtect(s, sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) return false;
    original = *s;
    *s = fn;
    VirtualProtect(s, sizeof(void *), old, &old);
    slot = s;
    return true;
}

void VtableHook::restore() {
    if (!slot) return;
    DWORD old;
    if (VirtualProtect(slot, sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
        *slot = original;
        VirtualProtect(slot, sizeof(void *), old, &old);
    }
    slot = nullptr;
}

// --- tables ------------------------------------------------------------------------------

namespace {

enum FieldType { FIELD_EMBEDDED = 10 };

struct datamap_t;
struct typedescription_t {
    int fieldType;
    const char *fieldName;
    int fieldOffset;
    unsigned short fieldSize;
    short flags;
    const char *externalName;
    void *pSaveRestoreOps;
    void *inputFunc;
    datamap_t *td;
    int fieldSizeInBytes;
    typedescription_t *override_field;
    int override_count;
    float fieldTolerance;
    int flatOffset[2];
    unsigned short flatGroup;
};
struct datamap_t {
    typedescription_t *dataDesc;
    int dataNumFields;
    const char *dataClassName;
    datamap_t *baseMap;
};

struct SendTable;
struct SendProp {
    void *vmt;
    void *matching;
    int type;  // 6 = DPT_DataTable
    int bits;
    float low, high;
    SendProp *arrayProp;
    void *arrayLengthProxy;
    int elements, elementStride;
    char *excludeName, *parentArrayName, *varName;
    float highLowMul;
    char priority;
    int flags;
    void *proxy, *dataTableProxy;
    SendTable *dataTable;
    int offset;
    const void *extra;
};
struct SendTable {
    SendProp *props;
    int count;
    char *name;
};
struct ServerClass {
    char *name;
    SendTable *table;
    ServerClass *next;
    int id;
};

struct RecvTable;
struct RecvProp {
    char *varName;
    int type, flags, stringBufferSize;
    bool insideArray;
    const void *extra;
    RecvProp *arrayProp;
    void *arrayLengthProxy, *proxy, *dataTableProxy;
    RecvTable *dataTable;
    int offset, elementStride, elements;
    const char *parentArrayName;
};
struct RecvTable {
    RecvProp *props;
    int count;
    void *decoder;
    char *name;
};
struct ClientClass {
    void *create, *createEvent;
    char *name;
    RecvTable *table;
    ClientClass *next;
    int id;
};

using FieldMap = std::unordered_map<std::string, int>;

void add_field(FieldMap &m, const std::string &name, int offset) { m.emplace(name, offset); }

void walk_datamap(const datamap_t *dm, int base, const std::string &prefix, FieldMap &out, int depth = 0) {
    for (; dm && depth < 32; dm = dm->baseMap, depth++) {
        for (int i = 0; i < dm->dataNumFields; i++) {
            const typedescription_t &f = dm->dataDesc[i];
            if (!f.fieldName) continue;
            int off = base + f.fieldOffset;
            if (f.fieldType == FIELD_EMBEDDED && f.td) {
                walk_datamap(f.td, off, prefix + f.fieldName + ".", out, depth + 1);
                // embedded members are also reachable without the prefix (m_Collision.m_vecMins as m_vecMins)
                walk_datamap(f.td, off, "", out, depth + 1);
            }
            add_field(out, prefix + f.fieldName, off);
        }
    }
}

void walk_sendtable(const SendTable *t, int base, FieldMap &out, int depth = 0) {
    if (!t || depth > 16) return;
    for (int i = 0; i < t->count; i++) {
        const SendProp &p = t->props[i];
        if (!p.varName) continue;
        if (p.type == 6 && p.dataTable) {
            walk_sendtable(p.dataTable, base + p.offset, out, depth + 1);
            continue;
        }
        add_field(out, p.varName, base + p.offset);
    }
}

bool readable(uintptr_t addr, size_t size);

void walk_recvtable(const RecvTable *t, int base, FieldMap &out, int depth = 0) {
    if (!t || depth > 16 || !readable(uintptr_t(t), sizeof *t)) return;
    if (t->count < 0 || t->count > 4096 || !readable(uintptr_t(t->props), sizeof(RecvProp) * t->count)) return;
    for (int i = 0; i < t->count; i++) {
        const RecvProp &p = t->props[i];
        if (!p.varName || !readable(uintptr_t(p.varName), 1)) continue;
        if (p.type == 6 && p.dataTable) {
            walk_recvtable(p.dataTable, base + p.offset, out, depth + 1);
            continue;
        }
        add_field(out, p.varName, base + p.offset);
    }
}

// server: vtable indices of CBaseEntity
constexpr int kGetServerClass = 9, kGetDataDescMap = 11;
// client: of C_BaseEntity (as IClientEntity)
constexpr int kClientGetDataDescMap = 15, kGetPredDescMap = 17;
// IClientNetworkable (at +8 in C_BaseEntity): GetClientClass is its third function
constexpr int kNetworkableOffset = 8, kGetClientClass = 2;

std::unordered_map<const void *, FieldMap> g_server_fields, g_client_fields;

const FieldMap &server_fields(void *ent) {
    auto *dm = vcall<datamap_t *>(ent, kGetDataDescMap);
    auto it = g_server_fields.find(dm);
    if (it != g_server_fields.end()) return it->second;
    FieldMap &m = g_server_fields[dm];
    walk_datamap(dm, 0, "", m);  // datamaps first: they have every saved field
    if (auto *sc = vcall<ServerClass *>(ent, kGetServerClass)) walk_sendtable(sc->table, 0, m);
    return m;
}

const FieldMap &client_fields(void *ent) {
    auto *dm = vcall<datamap_t *>(ent, kClientGetDataDescMap);
    auto it = g_client_fields.find(dm);
    if (it != g_client_fields.end()) return it->second;
    FieldMap &m = g_client_fields[dm];
    walk_datamap(dm, 0, "", m);
    walk_datamap(vcall<datamap_t *>(ent, kGetPredDescMap), 0, "", m);
    void *net = static_cast<char *>(ent) + kNetworkableOffset;
    auto *cc = vcall<ClientClass *>(net, kGetClientClass);
    if (cc && readable(uintptr_t(cc), sizeof *cc)) walk_recvtable(cc->table, 0, m);
    else fpvlog::write("sdk: no client class for %p", ent);
    return m;
}

struct CEntInfo {
    void *entity;
    int serial;
    CEntInfo *prev, *next;
    void *name, *classname;
};
constexpr int kEntEntries = 0x2000;

// Reads a pointer from game code or data, if the memory is there.
bool readable(uintptr_t addr, size_t size) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!addr || !VirtualQuery(reinterpret_cast<void *>(addr), &mbi, sizeof mbi)) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return addr + size <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}
template <typename T>
T read(uintptr_t addr) {
    return readable(addr, sizeof(T)) ? *reinterpret_cast<T *>(addr) : T{};
}

void *module_factory(const char *module, const char *name) {
    HMODULE h = GetModuleHandleA(module);
    if (!h) return nullptr;
    using Fn = void *(*)(const char *, int *);
    auto f = reinterpret_cast<Fn>(reinterpret_cast<void *>(GetProcAddress(h, "CreateInterface")));
    return f ? f(name, nullptr) : nullptr;
}

}  // namespace

// --- interfaces ------------------------------------------------------------------------------

bool init(void *(*engine_factory)(const char *, int *), void *(*server_factory)(const char *, int *)) {
    auto eng = [&](const char *n) { return engine_factory ? engine_factory(n, nullptr) : module_factory("engine.dll", n); };
    auto srv = [&](const char *n) { return server_factory ? server_factory(n, nullptr) : module_factory("server.dll", n); };
    I.engine_client = eng("VEngineClient015");
    I.engine_server = eng("VEngineServer022");
    I.engine_tool = eng("VENGINETOOL003");
    I.trace = eng("EngineTraceServer004");
    I.cvar = eng("VEngineCvar007");
    if (!I.cvar) I.cvar = module_factory("vstdlib.dll", "VEngineCvar007");
    I.client_dll = module_factory("client.dll", "VClient016");
    I.client_entities = module_factory("client.dll", "VClientEntityList003");
    I.server_tools = srv("VSERVERTOOLS001");
    I.server_game_dll = srv("ServerGameDLL005");
    I.player_info = srv("PlayerInfoManager002");
    if (I.player_info) I.globals = vcall<CGlobalVars *>(I.player_info, 1);  // GetGlobalVars

    // the server's entity list, read from IServerTools::GetIServerEntity's code
    if (I.server_tools) {
        auto fn = reinterpret_cast<uintptr_t>((*reinterpret_cast<void ***>(I.server_tools))[1]);
        I.ent_infos = read<void *>(fn + 61);
        if (!readable(reinterpret_cast<uintptr_t>(I.ent_infos), sizeof(CEntInfo) * kEntEntries)) I.ent_infos = nullptr;
    }
    // the client mode, from IBaseClientDLL::HudProcessInput: call GetClientMode, which returns g_pClientMode[0]
    if (I.client_dll) {
        auto hud = reinterpret_cast<uintptr_t>((*reinterpret_cast<void ***>(I.client_dll))[10]);
        uintptr_t call = hud + 4;
        if (read<uint8_t>(call - 1) == 0xE8) {  // a call rel32
            uintptr_t get_client_mode = call + read<int>(call) + 4;
            auto modes = read<uintptr_t>(get_client_mode + 19);
            I.client_mode = read<void *>(modes);
            if (!readable(read<uintptr_t>(reinterpret_cast<uintptr_t>(I.client_mode)), 64 * sizeof(void *)))
                I.client_mode = nullptr;  // no vtable there
        }
    }
    fpvlog::write("sdk: engine client %p server %p tool %p trace %p cvar %p client %p entities %p tools %p game %p globals %p "
                  "entity list %p client mode %p",
                  I.engine_client, I.engine_server, I.engine_tool, I.trace, I.cvar, I.client_dll, I.client_entities, I.server_tools,
                  I.server_game_dll, static_cast<void *>(I.globals), I.ent_infos, I.client_mode);
    return I.engine_client && I.trace && I.cvar && I.client_dll && I.client_entities && I.globals && I.ent_infos &&
           I.client_mode;
}

// --- engine helpers ------------------------------------------------------------------------

void exec(const char *cmd) {
    if (I.engine_client) vcall<void, const char *>(I.engine_client, 104, cmd);
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
void *find_command(const char *name) { return I.cvar ? vcall<void *, const char *>(I.cvar, 13, name) : nullptr; }
}  // namespace

bool uncheat(const char *name) {
    char *base = static_cast<char *>(find_command(name));
    if (!base) return false;
    *reinterpret_cast<int *>(base + 20) &= ~(1 << 14);  // ConCommandBase::m_nFlags, FCVAR_CHEAT
    return true;
}

std::string cvar_string(const char *name) {
    char *base = static_cast<char *>(find_command(name));
    if (!base) return "";
    char *parent = *reinterpret_cast<char **>(base + 28);  // ConVar::m_pParent
    if (!parent) parent = base;
    const char *s = *reinterpret_cast<const char **>(parent + 36);  // m_pszString
    return s ? s : "";
}

bool is_paused() { return I.engine_client && vcall<bool>(I.engine_client, 86); }

bool in_game() { return I.globals && player() != nullptr; }

void screen_size(int &w, int &h) {
    w = 1280, h = 720;
    if (I.engine_client) vcall<void, int *, int *>(I.engine_client, 5, &w, &h);
}

QAngle view_angles() {
    QAngle a;
    if (I.engine_client) vcall<void, QAngle *>(I.engine_client, 18, &a);
    return a;
}

void set_view_angles(const QAngle &a) {
    QAngle copy = a;
    if (I.engine_client) vcall<void, QAngle *>(I.engine_client, 19, &copy);
}

// --- server entities ----------------------------------------------------------------------

std::vector<void *> entities() {
    std::vector<void *> out;
    if (!I.ent_infos) return out;
    auto *infos = static_cast<CEntInfo *>(I.ent_infos);
    for (int i = 0; i < kEntEntries; i++)
        if (infos[i].entity) out.push_back(infos[i].entity);
    return out;
}

const char *classname(void *ent) {
    const char *s = get<const char *>(ent, "m_iClassname", nullptr);
    return s ? s : "";
}

const char *targetname(void *ent) {
    const char *s = get<const char *>(ent, "m_iName", nullptr);
    return s ? s : "";
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
    const uint32_t *h = vcall<const uint32_t *>(ent, 2);  // IHandleEntity::GetRefEHandle
    return h ? *h : 0xFFFFFFFFu;
}

int entity_index(void *ent) { return ent ? int(handle_of(ent) & 0xFFFF) : -1; }

void *entity_from_handle(uint32_t h) {
    if (h == 0xFFFFFFFFu || !I.ent_infos) return nullptr;
    int idx = int(h & 0xFFFF);
    if (idx >= kEntEntries) return nullptr;
    const CEntInfo &info = static_cast<CEntInfo *>(I.ent_infos)[idx];
    return info.entity && uint32_t(info.serial) == (h >> 16) ? info.entity : nullptr;
}

void *player() {
    if (!I.ent_infos) return nullptr;
    return static_cast<CEntInfo *>(I.ent_infos)[1].entity;
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
    if (!I.ent_infos || index < 0 || index >= kEntEntries) return nullptr;
    return static_cast<CEntInfo *>(I.ent_infos)[index].entity;
}

void *create_entity(const char *cls) {
    return I.server_tools ? vcall<void *, const char *>(I.server_tools, 14, cls) : nullptr;
}

bool set_keyvalue(void *ent, const char *key, const char *value) {
    return I.server_tools && ent && vcall<bool, void *, const char *, const char *>(I.server_tools, 13, ent, key, value);
}

void dispatch_spawn(void *ent) {
    if (I.server_tools && ent) vcall<void, void *>(I.server_tools, 15, ent);
}

bool readable_str(const char *s) { return readable(reinterpret_cast<uintptr_t>(s), 1); }

int precache_model(const char *path) {
    return I.engine_tool ? vcall<int, const char *, bool>(I.engine_tool, 61, path, true) : 0;
}

uintptr_t scan(const char *module, const char *pattern) {
    HMODULE h = GetModuleHandleA(module);
    if (!h) return 0;
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(h);
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(reinterpret_cast<uint8_t *>(h) + dos->e_lfanew);
    const uint8_t *base = reinterpret_cast<const uint8_t *>(h);
    size_t size = nt->OptionalHeader.SizeOfImage;
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
        while (j < bytes.size() && (bytes[j] < 0 || base[i + j] == bytes[j])) j++;
        if (j == bytes.size()) return reinterpret_cast<uintptr_t>(base + i);
    }
    return 0;
}

void *client_entity(int index) {
    return I.client_entities ? vcall<void *, int>(I.client_entities, 3, index) : nullptr;
}

int client_field_offset(void *ent, const char *name) {
    if (!ent) return -1;
    const FieldMap &m = client_fields(ent);
    auto it = m.find(name);
    return it == m.end() ? -1 : it->second;
}

// --- traces ------------------------------------------------------------------------------

bool TraceFilter::ShouldHitEntity(void *handle_entity, int) {
    for (int i = 0; i < skip_count; i++)
        if (skip[i] == handle_entity) return false;
    return true;
}

void trace_ray(const Vector &from, const Vector &to, uint32_t mask, TraceFilter *filter, CGameTrace &out) {
    std::memset(static_cast<void *>(&out), 0, sizeof out);
    out.fraction = 1;
    if (!I.trace) return;
    Ray ray(from, to);
    TraceFilter none;
    vcall<void, const Ray *, unsigned, TraceFilter *, CGameTrace *>(I.trace, 5, &ray, mask, filter ? filter : &none, &out);
}

}  // namespace sdk
