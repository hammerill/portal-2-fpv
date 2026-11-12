#include "render.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "drone.h"
#include "fpvapi.h"
#include "log.h"
#include "paths.h"
#include "platform.h"

namespace render {
namespace {

using fpvapi::fpv;

struct Vertex {
    float x, y, z, rhw;
    DWORD color;
    float u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1;

struct Texture {
    IDirect3DTexture9 *tex = nullptr;
    uint32_t revision = 0;
};
Texture g_textures[3];  // indexed by FPV_TEXTURE_* (1 overlay font, 2 OSD font)
std::vector<Vertex> g_vertices;
double g_last = 0;
std::atomic<float> g_aspect{16.0f / 9.0f};

// Fisheye: a copy of the finished frame, drawn back through a warped grid.
IDirect3DTexture9 *g_scene = nullptr;
UINT g_scene_w = 0, g_scene_h = 0;
D3DFORMAT g_scene_format = D3DFMT_UNKNOWN;
constexpr int kGridX = 64, kGridY = 36;  // cells; the warp is smooth, so this is exact to a pixel
std::vector<Vertex> g_grid;
std::vector<uint16_t> g_grid_indices;

// Signal lost: analog static, regenerated every frame and stretched to the screen.
constexpr UINT kStaticW = 320, kStaticH = 180;
IDirect3DTexture9 *g_static = nullptr;
uint32_t g_noise_seed = 0x2545f491u;

IDirect3DTexture9 *texture(IDirect3DDevice9 *dev, uint64_t id) {
    if (id >= 3) return nullptr;
    fpv_texture t{};
    if (fpv.fpv_texture_get(id, &t) != FPV_OK || !t.rgba) return nullptr;
    Texture &slot = g_textures[id];
    if (slot.tex && slot.revision == t.revision) return slot.tex;
    if (slot.tex) slot.tex->Release();
    slot.tex = nullptr;
    if (FAILED(dev->CreateTexture(t.width, t.height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &slot.tex, nullptr)))
        return nullptr;
    D3DLOCKED_RECT lr;
    if (SUCCEEDED(slot.tex->LockRect(0, &lr, nullptr, 0))) {
        for (uint32_t y = 0; y < t.height; ++y) {
            const uint8_t *src = t.rgba + size_t(y) * t.width * 4;
            uint8_t *dst = static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch;
            for (uint32_t x = 0; x < t.width; ++x, src += 4, dst += 4) {  // RGBA -> BGRA
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
                dst[3] = src[3];
            }
        }
        slot.tex->UnlockRect(0);
    }
    slot.revision = t.revision;
    return slot.tex;
}

// Fixed-function state for alpha-blended screen-space quads, as in Dear
// ImGui's DX9 backend.
void setup_state(IDirect3DDevice9 *dev, float w, float h) {
    D3DVIEWPORT9 vp = {0, 0, DWORD(w), DWORD(h), 0.0f, 1.0f};
    dev->SetViewport(&vp);
    dev->SetPixelShader(nullptr);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(kFvf);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_SHADEMODE, D3DSHADE_GOURAUD);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
    dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
    dev->SetRenderState(D3DRS_SRCBLENDALPHA, D3DBLEND_ONE);
    dev->SetRenderState(D3DRS_DESTBLENDALPHA, D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_RANGEFOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SPECULARENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPING, TRUE);
    dev->SetRenderState(D3DRS_LIGHTING, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
    dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
    dev->SetTextureStageState(0, D3DTSS_TEXTURETRANSFORMFLAGS, D3DTTFF_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
    dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
}

void draw(IDirect3DDevice9 *dev, const fpv_draw_data &dd) {
    for (uint32_t l = 0; l < dd.list_count; ++l) {
        const fpv_draw_list &list = dd.lists[l];
        g_vertices.resize(list.vertex_count);
        for (uint32_t i = 0; i < list.vertex_count; ++i) {
            const fpv_draw_vertex &s = list.vertices[i];
            uint32_t c = s.color;  // ImGui ABGR -> D3D ARGB
            c = (c & 0xff00ff00u) | ((c & 0xffu) << 16) | ((c >> 16) & 0xffu);
            g_vertices[i] = {s.pos[0] - 0.5f, s.pos[1] - 0.5f, 0.0f, 1.0f, c, s.uv[0], s.uv[1]};
        }
        for (uint32_t c = 0; c < list.command_count; ++c) {
            const fpv_draw_cmd &cmd = list.commands[c];
            if (!cmd.index_count || cmd.vertex_offset >= list.vertex_count) continue;
            RECT clip = {LONG(cmd.clip_rect[0]), LONG(cmd.clip_rect[1]), LONG(cmd.clip_rect[2]), LONG(cmd.clip_rect[3])};
            if (clip.right <= clip.left || clip.bottom <= clip.top) continue;
            dev->SetScissorRect(&clip);
            dev->SetTexture(0, texture(dev, cmd.texture_id));
            dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, list.vertex_count - cmd.vertex_offset,
                                        cmd.index_count / 3, list.indices + cmd.index_offset, D3DFMT_INDEX32,
                                        g_vertices.data() + cmd.vertex_offset, sizeof(Vertex));
        }
    }
}

// libfpv's lens (docs/INTEGRATION.md#lens-distortion) as a mesh instead of a
// pixel shader: each grid vertex at output position uv samples the copied
// scene at c * (1 + k1 r2 + k2 r2^2) / zoom, with |c| = 1 at the corners.
// The game drew the frame with the widened FOV from frame.camera.
bool draw_lens(IDirect3DDevice9 *dev, IDirect3DSurface9 *backbuffer, const D3DSURFACE_DESC &desc,
               const fpv_camera &cam) {
    if (!g_scene || g_scene_w != desc.Width || g_scene_h != desc.Height || g_scene_format != desc.Format) {
        if (g_scene) g_scene->Release();
        g_scene = nullptr;
        if (FAILED(dev->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT,
                                      &g_scene, nullptr)))
            return false;
        g_scene_w = desc.Width, g_scene_h = desc.Height, g_scene_format = desc.Format;
    }
    IDirect3DSurface9 *copy = nullptr;
    if (FAILED(g_scene->GetSurfaceLevel(0, &copy))) return false;
    HRESULT hr = dev->StretchRect(backbuffer, nullptr, copy, nullptr, D3DTEXF_NONE);
    copy->Release();
    if (FAILED(hr)) return false;

    const float w = float(desc.Width), h = float(desc.Height), a = w / h;
    const float sx = a / (0.5f * std::sqrt(a * a + 1)), sy = 1 / (0.5f * std::sqrt(a * a + 1));
    const float k1 = cam.distortion_k1, k2 = cam.distortion_k2, zoom = cam.distortion_zoom > 0 ? cam.distortion_zoom : 1;
    g_grid.resize((kGridX + 1) * (kGridY + 1));
    for (int y = 0; y <= kGridY; ++y)
        for (int x = 0; x <= kGridX; ++x) {
            float u = float(x) / kGridX, v = float(y) / kGridY;
            float cx = (u - 0.5f) * sx, cy = (v - 0.5f) * sy, r2 = cx * cx + cy * cy;
            float m = (1 + k1 * r2 + k2 * r2 * r2) / zoom;
            g_grid[y * (kGridX + 1) + x] = {u * w - 0.5f, v * h - 0.5f, 0.0f, 1.0f, 0xffffffffu,
                                            cx * m / sx + 0.5f, cy * m / sy + 0.5f};
        }
    if (g_grid_indices.empty())
        for (int y = 0; y < kGridY; ++y)
            for (int x = 0; x < kGridX; ++x) {
                uint16_t i = uint16_t(y * (kGridX + 1) + x), j = uint16_t(i + kGridX + 1);
                for (uint16_t k : {i, uint16_t(i + 1), j, uint16_t(i + 1), uint16_t(j + 1), j}) g_grid_indices.push_back(k);
            }
    RECT full = {0, 0, LONG(desc.Width), LONG(desc.Height)};
    dev->SetScissorRect(&full);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetTexture(0, g_scene);
    dev->DrawIndexedPrimitiveUP(D3DPT_TRIANGLELIST, 0, UINT(g_grid.size()), UINT(g_grid_indices.size() / 3),
                                g_grid_indices.data(), D3DFMT_INDEX16, g_grid.data(), sizeof(Vertex));
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    return true;
}

uint32_t noise() {
    g_noise_seed ^= g_noise_seed << 13, g_noise_seed ^= g_noise_seed >> 17, g_noise_seed ^= g_noise_seed << 5;
    return g_noise_seed;
}

// Grey snow with brighter and darker rolling bands, like an analog receiver
// that lost its transmitter.
bool draw_static(IDirect3DDevice9 *dev, float w, float h) {
    if (!g_static &&
        FAILED(dev->CreateTexture(kStaticW, kStaticH, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_static, nullptr)))
        return false;
    D3DLOCKED_RECT lr;
    if (FAILED(g_static->LockRect(0, &lr, nullptr, 0))) return false;
    static float band_phase = 0;
    band_phase += 0.07f + float(noise() & 0xff) / 255.0f * 0.05f;
    for (UINT y = 0; y < kStaticH; ++y) {
        float band = 0.75f + 0.25f * std::sin(float(y) * 0.045f + band_phase);
        if ((noise() & 63) == 0) band *= 1.35f;  // a torn line now and then
        uint32_t *row = reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch);
        for (UINT x = 0; x < kStaticW; ++x) {
            float v = float(noise() & 0xff) * band;
            uint32_t g = v > 255 ? 255 : uint32_t(v);
            row[x] = 0xff000000u | g << 16 | g << 8 | g;
        }
    }
    g_static->UnlockRect(0);
    Vertex quad[4] = {{-0.5f, -0.5f, 0, 1, 0xffffffffu, 0, 0},
                      {w - 0.5f, -0.5f, 0, 1, 0xffffffffu, 1, 0},
                      {-0.5f, h - 0.5f, 0, 1, 0xffffffffu, 0, 1},
                      {w - 0.5f, h - 0.5f, 0, 1, 0xffffffffu, 1, 1}};
    RECT full = {0, 0, LONG(w), LONG(h)};
    dev->SetScissorRect(&full);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetTexture(0, g_static);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(Vertex));
    dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
    dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    return true;
}

// Self-test screenshots: creating fpv-data/shot.txt saves the next finished
// frame (with our passes) to fpv-data/shots/<time>.bmp and deletes the file.
// Window capture from outside does not always work (a WSL in session 0).
void save_screenshot(IDirect3DDevice9 *dev, IDirect3DSurface9 *backbuffer, const D3DSURFACE_DESC &desc) {
    static int frames = 0;
    static const std::string data = paths::data_dir(), trigger = data + "/shot.txt";
    if (++frames % 10 != 0 || !platform::file_exists(trigger)) return;
    platform::remove_file(trigger);
    IDirect3DSurface9 *rt = nullptr, *mem = nullptr;
    bool ok = SUCCEEDED(dev->CreateRenderTarget(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                                                &rt, nullptr)) &&
              SUCCEEDED(dev->StretchRect(backbuffer, nullptr, rt, nullptr, D3DTEXF_NONE)) &&
              SUCCEEDED(dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM,
                                                        &mem, nullptr)) &&
              SUCCEEDED(dev->GetRenderTargetData(rt, mem));
    D3DLOCKED_RECT lr;
    if (ok && SUCCEEDED(mem->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        platform::make_dir(data + "/shots");
        int th, tm, ts, tms;
        platform::local_time(th, tm, ts, tms);
        char path[1024];
        snprintf(path, sizeof path, "%s/shots/%02d%02d%02d-%03d.bmp", data.c_str(), th, tm, ts, tms);
        if (FILE *f = std::fopen(path, "wb")) {
            // BITMAPFILEHEADER (14 bytes) and BITMAPINFOHEADER (40), little-endian; top-down rows
            uint8_t hdr[54] = {'B', 'M'};
            auto put = [&](int at, uint32_t v, int n) {
                for (int i = 0; i < n; ++i) hdr[at + i] = uint8_t(v >> (8 * i));
            };
            put(2, 54 + desc.Width * desc.Height * 4, 4);
            put(10, 54, 4);
            put(14, 40, 4);
            put(18, desc.Width, 4);
            put(22, uint32_t(-int32_t(desc.Height)), 4);
            put(26, 1, 2);
            put(28, 32, 2);
            std::fwrite(hdr, sizeof hdr, 1, f);
            for (UINT y = 0; y < desc.Height; ++y)
                std::fwrite(static_cast<uint8_t *>(lr.pBits) + size_t(y) * lr.Pitch, 4, desc.Width, f);
            std::fclose(f);
            fpvlog::write("render: screenshot %s", path);
        }
        mem->UnlockRect();
    } else {
        fpvlog::write("render: screenshot failed (format %d)", int(desc.Format));
    }
    if (mem) mem->Release();
    if (rt) rt->Release();
}

// Mouse for the overlay, in back buffer pixels.
void feed_mouse(float w, float h) {
    float x, y;
    if (!input::cursor(w, h, x, y)) return;
    fpv.fpv_overlay_mouse_move(x, y);
    static bool was[3];
    for (int b = 0; b < 3; ++b) {
        bool down = input::mouse_down(b);
        if (down != was[b]) fpv.fpv_overlay_mouse_button(b, down);
        was[b] = down;
    }
}

// Everything setup_state() and draw() change, so it can be put back.
const D3DRENDERSTATETYPE kRenderStates[] = {
    D3DRS_FILLMODE, D3DRS_SHADEMODE, D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_CULLMODE,
    D3DRS_ALPHABLENDENABLE, D3DRS_BLENDOP, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_SEPARATEALPHABLENDENABLE,
    D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_RANGEFOGENABLE,
    D3DRS_SPECULARENABLE, D3DRS_STENCILENABLE, D3DRS_CLIPPING, D3DRS_LIGHTING, D3DRS_SRGBWRITEENABLE,
    D3DRS_COLORWRITEENABLE, D3DRS_BLENDOPALPHA, D3DRS_ZFUNC,
};
const D3DTEXTURESTAGESTATETYPE kStage0States[] = {
    D3DTSS_COLOROP, D3DTSS_COLORARG1, D3DTSS_COLORARG2, D3DTSS_ALPHAOP, D3DTSS_ALPHAARG1, D3DTSS_ALPHAARG2,
    D3DTSS_TEXTURETRANSFORMFLAGS,
};
const D3DSAMPLERSTATETYPE kSamplerStates[] = {
    D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
};
constexpr size_t kNumRs = sizeof kRenderStates / sizeof kRenderStates[0];
constexpr size_t kNumTss = sizeof kStage0States / sizeof kStage0States[0];
constexpr size_t kNumSs = sizeof kSamplerStates / sizeof kSamplerStates[0];

struct SavedState {
    DWORD rs[kNumRs], tss0[kNumTss], tss1_color, tss1_alpha, ss[kNumSs];
    D3DVIEWPORT9 viewport;
    RECT scissor;
    IDirect3DBaseTexture9 *texture = nullptr;
    IDirect3DVertexShader9 *vs = nullptr;
    IDirect3DPixelShader9 *ps = nullptr;
    IDirect3DVertexDeclaration9 *decl = nullptr;
    DWORD fvf;
    IDirect3DVertexBuffer9 *stream = nullptr;
    UINT stream_offset, stream_stride;
    IDirect3DIndexBuffer9 *indices = nullptr;

    void save(IDirect3DDevice9 *dev) {
        for (size_t i = 0; i < kNumRs; ++i) dev->GetRenderState(kRenderStates[i], &rs[i]);
        for (size_t i = 0; i < kNumTss; ++i) dev->GetTextureStageState(0, kStage0States[i], &tss0[i]);
        dev->GetTextureStageState(1, D3DTSS_COLOROP, &tss1_color);
        dev->GetTextureStageState(1, D3DTSS_ALPHAOP, &tss1_alpha);
        for (size_t i = 0; i < kNumSs; ++i) dev->GetSamplerState(0, kSamplerStates[i], &ss[i]);
        dev->GetViewport(&viewport);
        dev->GetScissorRect(&scissor);
        dev->GetTexture(0, &texture);
        dev->GetVertexShader(&vs);
        dev->GetPixelShader(&ps);
        dev->GetVertexDeclaration(&decl);
        dev->GetFVF(&fvf);
        dev->GetStreamSource(0, &stream, &stream_offset, &stream_stride);
        dev->GetIndices(&indices);
    }

    // Releases the references save() took.
    void restore(IDirect3DDevice9 *dev) {
        for (size_t i = 0; i < kNumRs; ++i) dev->SetRenderState(kRenderStates[i], rs[i]);
        for (size_t i = 0; i < kNumTss; ++i) dev->SetTextureStageState(0, kStage0States[i], tss0[i]);
        dev->SetTextureStageState(1, D3DTSS_COLOROP, tss1_color);
        dev->SetTextureStageState(1, D3DTSS_ALPHAOP, tss1_alpha);
        for (size_t i = 0; i < kNumSs; ++i) dev->SetSamplerState(0, kSamplerStates[i], ss[i]);
        dev->SetViewport(&viewport);
        dev->SetScissorRect(&scissor);
        dev->SetTexture(0, texture);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        if (decl)
            dev->SetVertexDeclaration(decl);
        else
            dev->SetFVF(fvf);
        dev->SetStreamSource(0, stream, stream_offset, stream_stride);
        dev->SetIndices(indices);
        for (IUnknown *u : {static_cast<IUnknown *>(texture), static_cast<IUnknown *>(vs), static_cast<IUnknown *>(ps),
                            static_cast<IUnknown *>(decl), static_cast<IUnknown *>(stream),
                            static_cast<IUnknown *>(indices)})
            if (u) u->Release();
    }
};


}  // namespace

void frame(IDirect3DDevice9 *dev, bool in_game) {
    if (!fpvapi::loaded()) {
        IDirect3DSurface9 *backbuffer = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) return;
        D3DSURFACE_DESC desc;
        backbuffer->GetDesc(&desc);
        g_aspect = float(desc.Width) / float(desc.Height);
        save_screenshot(dev, backbuffer, desc);
        backbuffer->Release();
        return;
    }

    double now = platform::seconds();
    float dt = g_last > 0 ? float(now - g_last) : 0.016f;
    g_last = now;

    IDirect3DSurface9 *backbuffer = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer))) return;
    D3DSURFACE_DESC desc;
    backbuffer->GetDesc(&desc);
    float w = float(desc.Width), h = float(desc.Height);

    g_aspect = w / h;

    fpv.fpv_overlay_set_display(w, h, h > 1080.0f ? h / 1080.0f : 1.0f);
    if (fpv.fpv_overlay_is_open()) feed_mouse(w, h);

    static fpv_frame snapshot;
    const fpv_draw_data *osd = nullptr;
    bool no_signal = false;
    bool fpv_view = in_game && drone::osd_frame(&snapshot, &no_signal);
    bool lens = fpv_view && !no_signal && (snapshot.camera.distortion_k1 != 0 || snapshot.camera.distortion_k2 != 0);
    if (fpv_view)
        osd = fpv.fpv_osd_draw_data(&snapshot, w, h, FPV_OSD_STYLE_ANALOG);
    const fpv_draw_data *overlay = fpv.fpv_overlay_render(dt);
    if (!lens && !no_signal && (!osd || !osd->list_count) && (!overlay || !overlay->list_count)) {
        save_screenshot(dev, backbuffer, desc);
        backbuffer->Release();
        return;
    }

    // Draw straight onto the bound back buffer and restore exactly the state
    // we touch. A D3DSBT_ALL state block plus a render target / depth swap
    // here made the game's next frame flicker (stale regions, exposure jumps).
    IDirect3DSurface9 *rt = nullptr;
    dev->GetRenderTarget(0, &rt);
    bool on_backbuffer = rt == backbuffer;
    if (rt) rt->Release();
    if (!on_backbuffer) {
        backbuffer->Release();
        return;
    }

    SavedState saved;
    saved.save(dev);
    setup_state(dev, w, h);
    if (lens && !draw_lens(dev, backbuffer, desc, snapshot.camera)) {
        static int reported = 0;
        if (reported++ < 3) fpvlog::write("render: fisheye pass failed (format %d)", int(desc.Format));
    }
    if (no_signal && !draw_static(dev, w, h)) {
        static int reported = 0;
        if (reported++ < 3) fpvlog::write("render: static pass failed");
    }
    if (osd) draw(dev, *osd);
    if (overlay) draw(dev, *overlay);
    saved.restore(dev);
    save_screenshot(dev, backbuffer, desc);
    backbuffer->Release();
}

float aspect() { return g_aspect; }

void release() {
    if (g_scene) g_scene->Release();
    g_scene = nullptr;
    g_scene_w = g_scene_h = 0;
    if (g_static) g_static->Release();
    g_static = nullptr;
    for (Texture &t : g_textures) {
        if (t.tex) t.tex->Release();
        t = Texture{};
    }
}

}  // namespace render
