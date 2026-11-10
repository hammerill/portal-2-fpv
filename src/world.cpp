#include "world.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "fpvapi.h"
#include "gameplay.h"
#include "log.h"

namespace world {
namespace {

using fpvapi::fpv;
using sdk::Vector;

constexpr uint32_t kTraceMask = sdk::MASK_SOLID | sdk::CONTENTS_WATER | sdk::CONTENTS_SLIME | sdk::CONTENTS_PLAYERCLIP;
constexpr float kPortalHalfWidth = 32, kPortalHalfHeight = 56;  // Portal 2's portals, units

std::vector<PortalPair> g_pairs;
std::vector<fpv_portal_link> g_links_sent;
bool g_installed = false;

uint8_t material_of(const sdk::CGameTrace &tr) {
    if (tr.contents & (sdk::CONTENTS_WATER | sdk::CONTENTS_SLIME)) return kLiquid;
    if (tr.contents & sdk::CONTENTS_WINDOW) return kGlass;
    if (tr.contents & sdk::CONTENTS_GRATE) return kGrate;
    return kDefault;
}

int raycast(void *, const float o[3], const float d[3], float max_distance, fpv_raycast_hit *hit) {
    Vector from(o), dir(d);
    sdk::TraceFilter filter;
    gameplay::add_own_entities(filter);
    sdk::CGameTrace tr;
    sdk::trace_ray(from, from + dir * max_distance, kTraceMask, &filter, tr);
    if (tr.startsolid || tr.fraction >= 1.0f) return 0;
    hit->distance = max_distance * tr.fraction;
    tr.plane.normal.to(hit->normal);
    hit->material = material_of(tr);
    hit->object_id = object_id(tr.ent);
    return 1;
}

// A portal's pose for libfpv: forward out of the wall, up along its height.
fpv_portal to_fpv(void *portal) {
    fpv_portal p{};
    sdk::abs_origin(portal).to(p.position);
    Vector f, r, u;
    sdk::angle_vectors(sdk::abs_angles(portal), &f, &r, &u);
    // libfpv's rotation takes the host axes (right = -Y, up = Z, forward = X):
    // the columns of the matrix are where X, Y (left) and Z go
    Vector left = -r;
    float m[3][3] = {{f.x, left.x, u.x}, {f.y, left.y, u.y}, {f.z, left.z, u.z}};
    float tr = m[0][0] + m[1][1] + m[2][2];
    float *q = p.rotation;
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
    float hw = sdk::get<float>(portal, "m_fNetworkHalfWidth", 0), hh = sdk::get<float>(portal, "m_fNetworkHalfHeight", 0);
    p.half_width = hw > 1 ? hw : kPortalHalfWidth;
    p.half_height = hh > 1 ? hh : kPortalHalfHeight;
    return p;
}

bool same(const fpv_portal_link &a, const fpv_portal_link &b) { return std::memcmp(&a, &b, sizeof a) == 0; }

void sync_portals() {
    g_pairs.clear();
    std::vector<fpv_portal_link> links;
    std::vector<void *> portals = sdk::find_entities("prop_portal");
    for (void *p : portals) {
        if (!sdk::get<bool>(p, "m_bActivated")) continue;
        void *other = sdk::entity_from_handle(sdk::get<uint32_t>(p, "m_hLinkedPortal", 0xFFFFFFFFu));
        if (!other || !sdk::get<bool>(other, "m_bActivated")) continue;
        if (other < p) continue;  // each pair once
        g_pairs.push_back({p, other});
        if (links.size() < FPV_MAX_PORTAL_LINKS) links.push_back({to_fpv(p), to_fpv(other)});
    }
    bool changed = links.size() != g_links_sent.size();
    for (size_t i = 0; !changed && i < links.size(); i++) changed = !same(links[i], g_links_sent[i]);
    if (!changed) return;
    fpv_result r = fpv.fpv_portals_set(links.empty() ? nullptr : links.data(), uint32_t(links.size()));
    if (r != FPV_OK) fpvlog::write("world: portals_set: %s", fpv.fpv_last_error());
    else fpvlog::write("world: %u portal pair(s)", unsigned(links.size()));
    g_links_sent = links;
}

}  // namespace

void install() {
    if (g_installed || !fpvapi::loaded()) return;
    g_installed = true;
    fpv.fpv_world_set_raycast_callback(&raycast, nullptr);
    fpv_material m{0.2f, 0.6f, 0};
    fpv.fpv_material_set(kDefault, &m);
    fpv_material liquid{0.0f, 0.3f, FPV_MATERIAL_KILLS_PROPS};  // goo and water: a real quad doesn't survive them
    fpv.fpv_material_set(kLiquid, &liquid);
    fpv_material glass{0.2f, 0.4f, FPV_MATERIAL_TRANSPARENT_TO_VIDEO};
    fpv.fpv_material_set(kGlass, &glass);
    fpv_material grate{0.2f, 0.6f, FPV_MATERIAL_TRANSPARENT_TO_VIDEO};
    fpv.fpv_material_set(kGrate, &grate);
    fpv_material repulsion{0.95f, 0.3f, 0};  // blue gel: bounces
    fpv.fpv_material_set(kRepulsionGel, &repulsion);
    fpv_material propulsion{0.1f, 0.03f, 0};  // orange gel: slides
    fpv.fpv_material_set(kPropulsionGel, &propulsion);
}

void before_tick(float) {
    if (!fpvapi::loaded()) return;
    install();
    sync_portals();
}

void reset() {
    g_pairs.clear();
    if (fpvapi::loaded() && !g_links_sent.empty()) fpv.fpv_portals_set(nullptr, 0);
    g_links_sent.clear();
}

float near_plane(const Vector &pos, const Vector &fwd, const Vector &up, float aspect, float fov_y_deg) {
    float tv = std::tan(fov_y_deg * 3.14159265f / 360.0f), th = tv * std::max(aspect, 16.0f / 9.0f);
    Vector right = fwd.cross(up);
    sdk::TraceFilter filter;
    gameplay::add_own_entities(filter);
    float nearest = 1e9f;
    constexpr float kReach = 24;  // units: past that the default plane is fine
    for (int i = -1; i <= 1; i++)
        for (int j = -1; j <= 1; j++) {
            Vector dir = (fwd + right * (th * i) + up * (tv * j)).normalized();
            sdk::CGameTrace tr;
            sdk::trace_ray(pos, pos + dir * kReach, kTraceMask, &filter, tr);
            if (tr.fraction < 1 && !tr.startsolid) nearest = std::min(nearest, kReach * tr.fraction * dir.dot(fwd));
        }
    return std::clamp(nearest * 0.8f, 0.5f, 7.0f);
}

uint32_t object_id(void *ent) {
    if (!ent) return 0;
    int idx = sdk::entity_index(ent);
    if (idx <= 0) return 0;  // the world
    return uint32_t(idx);
}

void *object_entity(uint32_t id) { return id ? sdk::entity_by_index(int(id)) : nullptr; }

int portal_pairs(PortalPair *out, int max) {
    int n = std::min(max, int(g_pairs.size()));
    for (int i = 0; i < n; i++) out[i] = g_pairs[i];
    return n;
}

}  // namespace world
