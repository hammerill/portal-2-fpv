// The few Direct3D 9 declarations render.cpp needs, for macOS: the game's
// device there is mtld3d's (source-recomp's Metal renderer), a native arm64
// COM object. Vtable order as d3d9.h (checked against source-recomp's
// runtime/d3d9_generated.h); the methods render.cpp does not call are
// placeholders that keep the order.
#pragma once

#include <cstdint>

using DWORD = uint32_t;
using UINT = uint32_t;
using INT = int32_t;
using LONG = int32_t;
using BOOL = int32_t;
using HRESULT = int32_t;
using HANDLE = void *;
using HWND = void *;
using D3DFORMAT = uint32_t;
using D3DPOOL = uint32_t;
using D3DRESOURCETYPE = uint32_t;
using D3DMULTISAMPLE_TYPE = uint32_t;
using D3DTEXTUREFILTERTYPE = uint32_t;
using D3DBACKBUFFER_TYPE = uint32_t;
using D3DPRIMITIVETYPE = uint32_t;
using D3DRENDERSTATETYPE = uint32_t;
using D3DTEXTURESTAGESTATETYPE = uint32_t;
using D3DSAMPLERSTATETYPE = uint32_t;

#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif
#define FAILED(hr) (HRESULT(hr) < 0)
#define SUCCEEDED(hr) (HRESULT(hr) >= 0)

struct RECT {
    LONG left, top, right, bottom;
};
struct D3DVIEWPORT9 {
    DWORD X, Y, Width, Height;
    float MinZ, MaxZ;
};
struct D3DSURFACE_DESC {
    D3DFORMAT Format;
    D3DRESOURCETYPE Type;
    DWORD Usage;
    D3DPOOL Pool;
    D3DMULTISAMPLE_TYPE MultiSampleType;
    DWORD MultiSampleQuality;
    UINT Width, Height;
};
struct D3DLOCKED_RECT {
    INT Pitch;
    void *pBits;
};

enum : uint32_t {
    D3DFMT_UNKNOWN = 0, D3DFMT_A8R8G8B8 = 21, D3DFMT_X8R8G8B8 = 22, D3DFMT_INDEX16 = 101, D3DFMT_INDEX32 = 102,
    D3DPOOL_DEFAULT = 0, D3DPOOL_MANAGED = 1, D3DPOOL_SYSTEMMEM = 2,
    D3DUSAGE_RENDERTARGET = 1, D3DMULTISAMPLE_NONE = 0, D3DBACKBUFFER_TYPE_MONO = 0, D3DLOCK_READONLY = 0x10,
    D3DTEXF_NONE = 0, D3DTEXF_POINT = 1, D3DTEXF_LINEAR = 2,
    D3DPT_TRIANGLELIST = 4, D3DPT_TRIANGLESTRIP = 5,
    D3DFVF_XYZRHW = 0x004, D3DFVF_DIFFUSE = 0x040, D3DFVF_TEX1 = 0x100,
    // render states
    D3DRS_ZENABLE = 7, D3DRS_FILLMODE = 8, D3DRS_SHADEMODE = 9, D3DRS_ZWRITEENABLE = 14, D3DRS_ALPHATESTENABLE = 15,
    D3DRS_SRCBLEND = 19, D3DRS_DESTBLEND = 20, D3DRS_CULLMODE = 22, D3DRS_ZFUNC = 23, D3DRS_ALPHABLENDENABLE = 27,
    D3DRS_FOGENABLE = 28, D3DRS_SPECULARENABLE = 29, D3DRS_RANGEFOGENABLE = 48, D3DRS_STENCILENABLE = 52,
    D3DRS_CLIPPING = 136, D3DRS_LIGHTING = 137, D3DRS_COLORWRITEENABLE = 168, D3DRS_BLENDOP = 171,
    D3DRS_SCISSORTESTENABLE = 174, D3DRS_SRGBWRITEENABLE = 194, D3DRS_SEPARATEALPHABLENDENABLE = 206,
    D3DRS_SRCBLENDALPHA = 207, D3DRS_DESTBLENDALPHA = 208, D3DRS_BLENDOPALPHA = 209,
    D3DFILL_SOLID = 3, D3DSHADE_GOURAUD = 2, D3DCULL_NONE = 1, D3DBLENDOP_ADD = 1,
    D3DBLEND_ONE = 2, D3DBLEND_SRCALPHA = 5, D3DBLEND_INVSRCALPHA = 6,
    // texture stages
    D3DTSS_COLOROP = 1, D3DTSS_COLORARG1 = 2, D3DTSS_COLORARG2 = 3, D3DTSS_ALPHAOP = 4, D3DTSS_ALPHAARG1 = 5,
    D3DTSS_ALPHAARG2 = 6, D3DTSS_TEXTURETRANSFORMFLAGS = 24,
    D3DTOP_DISABLE = 1, D3DTOP_MODULATE = 4, D3DTA_DIFFUSE = 0, D3DTA_TEXTURE = 2, D3DTTFF_DISABLE = 0,
    // samplers
    D3DSAMP_ADDRESSU = 1, D3DSAMP_ADDRESSV = 2, D3DSAMP_MAGFILTER = 5, D3DSAMP_MINFILTER = 6, D3DSAMP_MIPFILTER = 7,
    D3DSAMP_SRGBTEXTURE = 11, D3DTADDRESS_CLAMP = 3,
};

struct IUnknown {
    virtual HRESULT QueryInterface(const void *iid, void **out) = 0;
    virtual DWORD AddRef() = 0;
    virtual DWORD Release() = 0;
};
struct IDirect3DResource9 : IUnknown {
    virtual void unused_GetDevice() = 0;
    virtual void unused_SetPrivateData() = 0;
    virtual void unused_GetPrivateData() = 0;
    virtual void unused_FreePrivateData() = 0;
    virtual void unused_SetPriority() = 0;
    virtual void unused_GetPriority() = 0;
    virtual void unused_PreLoad() = 0;
    virtual void unused_GetType() = 0;
};
struct IDirect3DSurface9 : IDirect3DResource9 {
    virtual void unused_GetContainer() = 0;                                // 11
    virtual HRESULT GetDesc(D3DSURFACE_DESC *desc) = 0;                    // 12
    virtual HRESULT LockRect(D3DLOCKED_RECT *rect, const RECT *area, DWORD flags) = 0;
    virtual HRESULT UnlockRect() = 0;                                      // 14
};
struct IDirect3DBaseTexture9 : IDirect3DResource9 {
    virtual void unused_SetLOD() = 0;  // 11
    virtual void unused_GetLOD() = 0;
    virtual void unused_GetLevelCount() = 0;
    virtual void unused_SetAutoGenFilterType() = 0;
    virtual void unused_GetAutoGenFilterType() = 0;
    virtual void unused_GenerateMipSubLevels() = 0;  // 16
};
struct IDirect3DTexture9 : IDirect3DBaseTexture9 {
    virtual void unused_GetLevelDesc() = 0;                                      // 17
    virtual HRESULT GetSurfaceLevel(UINT level, IDirect3DSurface9 **out) = 0;    // 18
    virtual HRESULT LockRect(UINT level, D3DLOCKED_RECT *rect, const RECT *area, DWORD flags) = 0;
    virtual HRESULT UnlockRect(UINT level) = 0;                                  // 20
};
struct IDirect3DVertexShader9 : IUnknown {};
struct IDirect3DPixelShader9 : IUnknown {};
struct IDirect3DVertexDeclaration9 : IUnknown {};
struct IDirect3DVertexBuffer9 : IUnknown {};
struct IDirect3DIndexBuffer9 : IUnknown {};

struct IDirect3DDevice9 : IUnknown {
    virtual void unused_TestCooperativeLevel() = 0;  // 3
    virtual void unused_GetAvailableTextureMem() = 0;  // 4
    virtual void unused_EvictManagedResources() = 0;  // 5
    virtual void unused_GetDirect3D() = 0;  // 6
    virtual void unused_GetDeviceCaps() = 0;  // 7
    virtual void unused_GetDisplayMode() = 0;  // 8
    virtual void unused_GetCreationParameters() = 0;  // 9
    virtual void unused_SetCursorProperties() = 0;  // 10
    virtual void unused_SetCursorPosition() = 0;  // 11
    virtual void unused_ShowCursor() = 0;  // 12
    virtual void unused_CreateAdditionalSwapChain() = 0;  // 13
    virtual void unused_GetSwapChain() = 0;  // 14
    virtual void unused_GetNumberOfSwapChains() = 0;  // 15
    virtual void unused_Reset() = 0;  // 16
    virtual void unused_Present() = 0;  // 17
    virtual HRESULT GetBackBuffer(UINT iSwapChain, UINT iBackBuffer, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out) = 0;  // 18
    virtual void unused_GetRasterStatus() = 0;  // 19
    virtual void unused_SetDialogBoxMode() = 0;  // 20
    virtual void unused_SetGammaRamp() = 0;  // 21
    virtual void unused_GetGammaRamp() = 0;  // 22
    virtual HRESULT CreateTexture(UINT width, UINT height, UINT levels, DWORD usage, D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9 **out, HANDLE *shared) = 0;  // 23
    virtual void unused_CreateVolumeTexture() = 0;  // 24
    virtual void unused_CreateCubeTexture() = 0;  // 25
    virtual void unused_CreateVertexBuffer() = 0;  // 26
    virtual void unused_CreateIndexBuffer() = 0;  // 27
    virtual HRESULT CreateRenderTarget(UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE ms, DWORD quality, BOOL lockable, IDirect3DSurface9 **out, HANDLE *shared) = 0;  // 28
    virtual void unused_CreateDepthStencilSurface() = 0;  // 29
    virtual void unused_UpdateSurface() = 0;  // 30
    virtual void unused_UpdateTexture() = 0;  // 31
    virtual HRESULT GetRenderTargetData(IDirect3DSurface9 *target, IDirect3DSurface9 *dest) = 0;  // 32
    virtual void unused_GetFrontBufferData() = 0;  // 33
    virtual HRESULT StretchRect(IDirect3DSurface9 *src, const RECT *src_rect, IDirect3DSurface9 *dst, const RECT *dst_rect, D3DTEXTUREFILTERTYPE filter) = 0;  // 34
    virtual void unused_ColorFill() = 0;  // 35
    virtual HRESULT CreateOffscreenPlainSurface(UINT width, UINT height, D3DFORMAT format, D3DPOOL pool, IDirect3DSurface9 **out, HANDLE *shared) = 0;  // 36
    virtual void unused_SetRenderTarget() = 0;  // 37
    virtual HRESULT GetRenderTarget(DWORD index, IDirect3DSurface9 **out) = 0;  // 38
    virtual void unused_SetDepthStencilSurface() = 0;  // 39
    virtual void unused_GetDepthStencilSurface() = 0;  // 40
    virtual void unused_BeginScene() = 0;  // 41
    virtual void unused_EndScene() = 0;  // 42
    virtual void unused_Clear() = 0;  // 43
    virtual void unused_SetTransform() = 0;  // 44
    virtual void unused_GetTransform() = 0;  // 45
    virtual void unused_MultiplyTransform() = 0;  // 46
    virtual HRESULT SetViewport(const D3DVIEWPORT9 *vp) = 0;  // 47
    virtual HRESULT GetViewport(D3DVIEWPORT9 *vp) = 0;  // 48
    virtual void unused_SetMaterial() = 0;  // 49
    virtual void unused_GetMaterial() = 0;  // 50
    virtual void unused_SetLight() = 0;  // 51
    virtual void unused_GetLight() = 0;  // 52
    virtual void unused_LightEnable() = 0;  // 53
    virtual void unused_GetLightEnable() = 0;  // 54
    virtual void unused_SetClipPlane() = 0;  // 55
    virtual void unused_GetClipPlane() = 0;  // 56
    virtual HRESULT SetRenderState(D3DRENDERSTATETYPE state, DWORD value) = 0;  // 57
    virtual HRESULT GetRenderState(D3DRENDERSTATETYPE state, DWORD *value) = 0;  // 58
    virtual void unused_CreateStateBlock() = 0;  // 59
    virtual void unused_BeginStateBlock() = 0;  // 60
    virtual void unused_EndStateBlock() = 0;  // 61
    virtual void unused_SetClipStatus() = 0;  // 62
    virtual void unused_GetClipStatus() = 0;  // 63
    virtual HRESULT GetTexture(DWORD stage, IDirect3DBaseTexture9 **out) = 0;  // 64
    virtual HRESULT SetTexture(DWORD stage, IDirect3DBaseTexture9 *texture) = 0;  // 65
    virtual HRESULT GetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD *value) = 0;  // 66
    virtual HRESULT SetTextureStageState(DWORD stage, D3DTEXTURESTAGESTATETYPE type, DWORD value) = 0;  // 67
    virtual HRESULT GetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD *value) = 0;  // 68
    virtual HRESULT SetSamplerState(DWORD sampler, D3DSAMPLERSTATETYPE type, DWORD value) = 0;  // 69
    virtual void unused_ValidateDevice() = 0;  // 70
    virtual void unused_SetPaletteEntries() = 0;  // 71
    virtual void unused_GetPaletteEntries() = 0;  // 72
    virtual void unused_SetCurrentTexturePalette() = 0;  // 73
    virtual void unused_GetCurrentTexturePalette() = 0;  // 74
    virtual HRESULT SetScissorRect(const RECT *rect) = 0;  // 75
    virtual HRESULT GetScissorRect(RECT *rect) = 0;  // 76
    virtual void unused_SetSoftwareVertexProcessing() = 0;  // 77
    virtual void unused_GetSoftwareVertexProcessing() = 0;  // 78
    virtual void unused_SetNPatchMode() = 0;  // 79
    virtual void unused_GetNPatchMode() = 0;  // 80
    virtual void unused_DrawPrimitive() = 0;  // 81
    virtual void unused_DrawIndexedPrimitive() = 0;  // 82
    virtual HRESULT DrawPrimitiveUP(D3DPRIMITIVETYPE type, UINT count, const void *vertices, UINT stride) = 0;  // 83
    virtual HRESULT DrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type, UINT min_index, UINT vertex_count, UINT count, const void *indices, D3DFORMAT index_format, const void *vertices, UINT stride) = 0;  // 84
    virtual void unused_ProcessVertices() = 0;  // 85
    virtual void unused_CreateVertexDeclaration() = 0;  // 86
    virtual HRESULT SetVertexDeclaration(IDirect3DVertexDeclaration9 *decl) = 0;  // 87
    virtual HRESULT GetVertexDeclaration(IDirect3DVertexDeclaration9 **decl) = 0;  // 88
    virtual HRESULT SetFVF(DWORD fvf) = 0;  // 89
    virtual HRESULT GetFVF(DWORD *fvf) = 0;  // 90
    virtual void unused_CreateVertexShader() = 0;  // 91
    virtual HRESULT SetVertexShader(IDirect3DVertexShader9 *shader) = 0;  // 92
    virtual HRESULT GetVertexShader(IDirect3DVertexShader9 **shader) = 0;  // 93
    virtual void unused_SetVertexShaderConstantF() = 0;  // 94
    virtual void unused_GetVertexShaderConstantF() = 0;  // 95
    virtual void unused_SetVertexShaderConstantI() = 0;  // 96
    virtual void unused_GetVertexShaderConstantI() = 0;  // 97
    virtual void unused_SetVertexShaderConstantB() = 0;  // 98
    virtual void unused_GetVertexShaderConstantB() = 0;  // 99
    virtual HRESULT SetStreamSource(UINT stream, IDirect3DVertexBuffer9 *buffer, UINT offset, UINT stride) = 0;  // 100
    virtual HRESULT GetStreamSource(UINT stream, IDirect3DVertexBuffer9 **buffer, UINT *offset, UINT *stride) = 0;  // 101
    virtual void unused_SetStreamSourceFreq() = 0;  // 102
    virtual void unused_GetStreamSourceFreq() = 0;  // 103
    virtual HRESULT SetIndices(IDirect3DIndexBuffer9 *indices) = 0;  // 104
    virtual HRESULT GetIndices(IDirect3DIndexBuffer9 **indices) = 0;  // 105
    virtual void unused_CreatePixelShader() = 0;  // 106
    virtual HRESULT SetPixelShader(IDirect3DPixelShader9 *shader) = 0;  // 107
    virtual HRESULT GetPixelShader(IDirect3DPixelShader9 **shader) = 0;  // 108
    virtual void unused_SetPixelShaderConstantF() = 0;  // 109
    virtual void unused_GetPixelShaderConstantF() = 0;  // 110
    virtual void unused_SetPixelShaderConstantI() = 0;  // 111
    virtual void unused_GetPixelShaderConstantI() = 0;  // 112
    virtual void unused_SetPixelShaderConstantB() = 0;  // 113
    virtual void unused_GetPixelShaderConstantB() = 0;  // 114
    virtual void unused_DrawRectPatch() = 0;  // 115
    virtual void unused_DrawTriPatch() = 0;  // 116
    virtual void unused_DeletePatch() = 0;  // 117
    virtual void unused_CreateQuery() = 0;  // 118
};
