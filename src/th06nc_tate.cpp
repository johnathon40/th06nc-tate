// th06nc-tate: THRotator-style tate mod for Touhou 6 New Classic (D3D11).
// Proxy dxgi.dll. Hooks IDXGISwapChain::Present and redraws the frame rotated,
// with the HUD rearranged around a magnified playfield during gameplay.
// Build with mingw-w64 (see build.sh). The Alt+0 menu uses Dear ImGui.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include "imgui.h"
#include "imgui_impl_dx11.h"

template <class T> static void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

// ---------------------------------------------------------------- config
struct Rect8 { int sx, sy, sw, sh, dx, dy, dw, dh; };
static const int kMaxRects = 32;

static char g_dir[MAX_PATH];
static char g_iniPath[MAX_PATH];
static char g_logPath[MAX_PATH];
static char g_bgName[MAX_PATH];

static int   g_rotation;          // 0..3 quarter turns
static int   g_filter = 1;        // 0 point, 1 linear
static int   g_autoDetect = 1;    // gameplay detection by the playfield frame
static int   g_defaultLayout = 1; // used when auto detect is off
static int   g_debug;
static int   g_openAtStartCfg;
static bool  g_ovVisible;         // in-game menu shown
static int   g_force = -1;        // -1 none, 0 whole screen, 1 HUD layout (Alt+Up/Down, not saved)
static int   g_featherX = 24, g_featherY = 10; // HUD edge fade, layout reference pixels
static int   g_portraitDesktop = -1; // non-16:9 desktop handling: -1 auto, 0 off, 1 on
static int   g_screenShape;          // physical screen shape: 0 same as resolution, 1 = 4:3, 2 = 16:9
static int   g_refW = 2560, g_refH = 1440;   // source reference (normal game screen)
static int   g_canW = 1440, g_canH = 1920;   // layout canvas reference (rotated, 3:4)
static Rect8 g_rects[kMaxRects];
static int   g_numRects;
static CRITICAL_SECTION g_lock;   // guards g_rects / g_numRects / g_autoDetect (editor thread)

static int parseInts(const char* s, int* out, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        if (!*s || *s == ';') break;
        int neg = 0, v = 0, any = 0;
        if (*s == '-') { neg = 1; s++; }
        while (*s >= '0' && *s <= '9') { v = v * 10 + (*s - '0'); s++; any = 1; }
        if (!any) break;
        out[n++] = neg ? -v : v;
    }
    return n;
}

static int iniInt(const char* sec, const char* key, int def) {
    return (int)GetPrivateProfileIntA(sec, key, def, g_iniPath);
}

static void loadConfig() {
    EnterCriticalSection(&g_lock);
    g_rotation      = iniInt("General", "Rotation", 0) & 3;
    g_filter        = iniInt("General", "Filter", 1);
    g_autoDetect    = iniInt("General", "AutoDetect", 1);
    g_defaultLayout = iniInt("General", "DefaultLayout", 1);
    g_debug         = iniInt("General", "Debug", 0);
    g_featherX      = iniInt("Layout", "FeatherX", 24);
    g_featherY      = iniInt("Layout", "FeatherY", 10);
    g_portraitDesktop = iniInt("General", "PortraitDesktop", -1);
    g_screenShape = iniInt("General", "ScreenShape", 0);
    if (g_screenShape < 0 || g_screenShape > 2) g_screenShape = 0;
    g_openAtStartCfg = iniInt("General", "OpenAtStart", 0);
    g_refW = iniInt("Layout", "RefWidth", 2560);
    g_refH = iniInt("Layout", "RefHeight", 1440);
    g_canW = iniInt("Layout", "CanvasWidth", 1440);
    g_canH = iniInt("Layout", "CanvasHeight", 1920);
    if (g_refW <= 0) g_refW = 2560;
    if (g_refH <= 0) g_refH = 1440;
    if (g_canW <= 0) g_canW = 1440;
    if (g_canH <= 0) g_canH = 1920;
    GetPrivateProfileStringA("Layout", "Background", "", g_bgName, MAX_PATH, g_iniPath);

    g_numRects = 0;
    for (int i = 0; i < kMaxRects; i++) {
        char key[16], buf[256];
        wsprintfA(key, "Rect%d", i);
        GetPrivateProfileStringA("Layout", key, "", buf, sizeof(buf), g_iniPath);
        int v[8];
        if (parseInts(buf, v, 8) == 8 && v[2] > 0 && v[3] > 0 && v[6] > 0 && v[7] > 0) {
            Rect8& r = g_rects[g_numRects++];
            r.sx = v[0]; r.sy = v[1]; r.sw = v[2]; r.sh = v[3];
            r.dx = v[4]; r.dy = v[5]; r.dw = v[6]; r.dh = v[7];
        }
    }
    LeaveCriticalSection(&g_lock);
}

static void saveRotation() {
    char buf[8];
    wsprintfA(buf, "%d", g_rotation);
    WritePrivateProfileStringA("General", "Rotation", buf, g_iniPath);
}

static void saveLayout() {
    EnterCriticalSection(&g_lock);
    char key[16], buf[160];
    for (int i = 0; i < kMaxRects; i++) {
        wsprintfA(key, "Rect%d", i);
        if (i < g_numRects) {
            const Rect8& r = g_rects[i];
            wsprintfA(buf, "%d,%d,%d,%d, %d,%d,%d,%d", r.sx, r.sy, r.sw, r.sh, r.dx, r.dy, r.dw, r.dh);
            WritePrivateProfileStringA("Layout", key, buf, g_iniPath);
        } else {
            WritePrivateProfileStringA("Layout", key, nullptr, g_iniPath); // delete leftovers
        }
    }
    wsprintfA(buf, "%d", g_autoDetect);
    WritePrivateProfileStringA("General", "AutoDetect", buf, g_iniPath);
    LeaveCriticalSection(&g_lock);
}

static void writeIntKey(const char* sec, const char* key, int v) {
    char b[16];
    wsprintfA(b, "%d", v);
    WritePrivateProfileStringA(sec, key, b, g_iniPath);
}

static void saveAll() {
    saveLayout();
    writeIntKey("General", "Rotation", g_rotation);
    writeIntKey("General", "Filter", g_filter);
    writeIntKey("General", "OpenAtStart", g_openAtStartCfg);
    writeIntKey("General", "AutoDetect", g_autoDetect);
    writeIntKey("General", "DefaultLayout", g_defaultLayout);
    writeIntKey("General", "Debug", g_debug);
    writeIntKey("General", "PortraitDesktop", g_portraitDesktop);
    writeIntKey("General", "ScreenShape", g_screenShape);
    writeIntKey("Layout", "FeatherX", g_featherX);
    writeIntKey("Layout", "FeatherY", g_featherY);
    writeIntKey("Layout", "RefWidth", g_refW);
    writeIntKey("Layout", "RefHeight", g_refH);
    writeIntKey("Layout", "CanvasWidth", g_canW);
    writeIntKey("Layout", "CanvasHeight", g_canH);
    WritePrivateProfileStringA("Layout", "Background", g_bgName[0] ? g_bgName : nullptr, g_iniPath);
}

static volatile bool g_samplerDirty, g_bgDirty; // set by the settings window, applied on the render thread

// Factory settings (rotation is kept). Takes effect immediately; Save writes it to the ini.
static void resetDefaults() {
    static const Rect8 def[] = {
        { 708, 48, 1152, 1344, 0, 120, 1440, 1680 },  // playfield
        { 1950, 158, 560, 62, 8, 4, 506, 56 },        // hi score
        { 1950, 233, 560, 62, 524, 4, 506, 56 },      // score
        { 1950, 620, 560, 62, 8, 62, 506, 56 },       // graze
        { 1950, 695, 560, 62, 524, 62, 506, 56 },     // points
        { 2000, 56, 420, 74, 1040, 26, 392, 69 },     // difficulty (wide enough for LUNATIC)
        { 1950, 350, 580, 62, 8, 1806, 524, 56 },     // player
        { 1950, 423, 580, 62, 540, 1806, 524, 56 },   // bomb
        { 1950, 538, 590, 66, 8, 1862, 500, 56 },     // power
        { 2260, 1388, 190, 44, 1250, 1870, 173, 40 }, // fps
    };
    EnterCriticalSection(&g_lock);
    g_numRects = sizeof(def) / sizeof(def[0]);
    for (int i = 0; i < g_numRects; i++) g_rects[i] = def[i];
    g_filter = 1; g_autoDetect = 1; g_defaultLayout = 1; g_debug = 0;
    g_portraitDesktop = -1; g_screenShape = 0; g_openAtStartCfg = 0;
    g_refW = 2560; g_refH = 1440; g_canW = 1440; g_canH = 1920;
    g_featherX = 24; g_featherY = 10;
    lstrcpyA(g_bgName, "th06nc-tate-bg.bmp");
    g_force = -1;
    LeaveCriticalSection(&g_lock);
    g_samplerDirty = true;
    g_bgDirty = true;
}

static void logLine(const char* s) {
    HANDLE f = CreateFileA(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w;
    WriteFile(f, s, lstrlenA(s), &w, nullptr);
    WriteFile(f, "\r\n", 2, &w, nullptr);
    CloseHandle(f);
}

// ---------------------------------------------------------------- vtable patching
static void* patchVT(void* obj, int idx, void* fn) {
    void** vt = *(void***)obj;
    if (vt[idx] == fn) return nullptr; // already ours
    DWORD old;
    VirtualProtect(&vt[idx], sizeof(void*), PAGE_EXECUTE_READWRITE, &old);
    void* prev = vt[idx];
    vt[idx] = fn;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return prev;
}

// ---------------------------------------------------------------- renderer state
struct Vtx { float x, y, u, v, lx, ly, fx, fy; }; // pos, tex, local 0..1, feather (fraction of quad)
static const int kMaxVerts = (kMaxRects + 2) * 6;

static ID3D11Device*             g_dev;
static ID3D11DeviceContext*      g_ctx;
static ID3D11VertexShader*       g_vs;
static ID3D11PixelShader*        g_ps;
static ID3D11InputLayout*        g_il;
static ID3D11Buffer*             g_vb;
static ID3D11SamplerState*       g_smp;
static ID3D11RasterizerState*    g_rs;
static ID3D11BlendState*         g_bs;
static ID3D11DepthStencilState*  g_dss;
static ID3D11Texture2D*          g_copy;
static ID3D11ShaderResourceView* g_copySrv;
static ID3D11ShaderResourceView* g_bgSrv;
static ID3D11Texture2D*          g_stage[3];
static bool g_stageFilled[3];
static int  g_stageIdx;
static UINT g_copyW, g_copyH;
static DXGI_FORMAT g_copyFmt;
static DXGI_FORMAT g_stageFmt = DXGI_FORMAT_UNKNOWN;
// current frame source (game image) and output (real backbuffer)
static ID3D11Texture2D* g_srcTex;
static UINT g_srcW, g_srcH;
static DXGI_FORMAT g_srcFmt;
// portrait desktop: the game draws into this fake landscape backbuffer
static bool g_portraitOut;
static ID3D11Texture2D* g_fake;
static ID3D11ShaderResourceView* g_fakeSrv;
static UINT g_fakeW, g_fakeH;
static bool g_initFailed;

static const char kShader[] =
    "struct VI{float2 p:POSITION;float2 t:TEXCOORD0;float2 l:TEXCOORD1;float2 f:TEXCOORD2;};"
    "struct VO{float4 p:SV_Position;float2 t:TEXCOORD0;float2 l:TEXCOORD1;float2 f:TEXCOORD2;};"
    "VO vs(VI i){VO o;o.p=float4(i.p,0,1);o.t=i.t;o.l=i.l;o.f=i.f;return o;}"
    "Texture2D tx:register(t0);SamplerState ss:register(s0);"
    "float4 ps(VO i):SV_Target{"
    "float2 d=min(i.l,1-i.l);float a=1;"
    "if(i.f.x>0)a*=smoothstep(0,1,saturate(d.x/i.f.x));"
    "if(i.f.y>0)a*=smoothstep(0,1,saturate(d.y/i.f.y));"
    "return float4(tx.Sample(ss,i.t).rgb,a);}";

static DXGI_FORMAT typedFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:    return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:    return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:    return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return f;
    }
}

// ---------------------------------------------------------------- background image (BMP, 24/32-bit)
static void loadBackground() {
    if (!g_bgName[0]) return;
    char path[MAX_PATH];
    lstrcpyA(path, g_dir);
    lstrcatA(path, g_bgName);
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { logLine("th06nc-tate: background image not found"); return; }
    DWORD size = GetFileSize(f, nullptr), got = 0;
    unsigned char* file = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, size);
    if (!file) { CloseHandle(f); return; }
    ReadFile(f, file, size, &got, nullptr);
    CloseHandle(f);

    BITMAPFILEHEADER* fh = (BITMAPFILEHEADER*)file;
    BITMAPINFOHEADER* ih = (BITMAPINFOHEADER*)(file + sizeof(BITMAPFILEHEADER));
    if (got < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) || fh->bfType != 0x4D42 ||
        (ih->biBitCount != 24 && ih->biBitCount != 32) || (ih->biCompression != BI_RGB && ih->biCompression != BI_BITFIELDS)) {
        logLine("th06nc-tate: background must be an uncompressed 24 or 32-bit BMP");
        HeapFree(GetProcessHeap(), 0, file);
        return;
    }
    int w = ih->biWidth, h = ih->biHeight > 0 ? ih->biHeight : -ih->biHeight;
    bool bottomUp = ih->biHeight > 0;
    int bpp = ih->biBitCount / 8;
    int stride = ((w * bpp) + 3) & ~3;
    if (fh->bfOffBits + (DWORD)stride * h > got) { HeapFree(GetProcessHeap(), 0, file); return; }

    unsigned char* px = (unsigned char*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
    if (px) {
        for (int y = 0; y < h; y++) {
            const unsigned char* src = file + fh->bfOffBits + (SIZE_T)stride * (bottomUp ? h - 1 - y : y);
            unsigned char* dst = px + (SIZE_T)w * 4 * y;
            for (int x = 0; x < w; x++) {
                dst[x * 4 + 0] = src[x * bpp + 0];
                dst[x * 4 + 1] = src[x * bpp + 1];
                dst[x * 4 + 2] = src[x * bpp + 2];
                dst[x * 4 + 3] = 255;
            }
        }
        D3D11_TEXTURE2D_DESC d = {};
        d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_IMMUTABLE;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA sd = { px, (UINT)w * 4, 0 };
        ID3D11Texture2D* tex = nullptr;
        if (SUCCEEDED(g_dev->CreateTexture2D(&d, &sd, &tex))) {
            g_dev->CreateShaderResourceView(tex, nullptr, &g_bgSrv);
            tex->Release();
        }
        HeapFree(GetProcessHeap(), 0, px);
    }
    HeapFree(GetProcessHeap(), 0, file);
}

// ---- portrait desktop: the game clips to its real window size; widen those clips to the fake buffer
typedef void (STDMETHODCALLTYPE* PFN_RSSetViewports)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void (STDMETHODCALLTYPE* PFN_RSSetScissorRects)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
static PFN_RSSetViewports    o_RSSetViewports;
static PFN_RSSetScissorRects o_RSSetScissorRects;
static UINT g_outW, g_outH;     // real output buffer size (non-16:9 desktop)
static UINT g_fakeWv, g_fakeHv; // fake (game) buffer size, mirrored here for the viewport hooks
static int  g_vpLogs, g_scLogs;
static bool g_portraitOutFlag;  // mirrors g_portraitOut, set later
static bool g_ovDrawing;        // our menu is drawing: leave its viewports and clips alone

static void STDMETHODCALLTYPE hkRSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    if (g_portraitOutFlag && !g_ovDrawing && v && n > 0 && n <= 16) {
        D3D11_VIEWPORT t[16];
        for (UINT i = 0; i < n; i++) {
            t[i] = v[i];
            if (g_vpLogs < 8) {
                g_vpLogs++;
                char b[128];
                wsprintfA(b, "th06nc-tate: viewport %d,%d %dx%d", (int)v[i].TopLeftX, (int)v[i].TopLeftY, (int)v[i].Width, (int)v[i].Height);
                logLine(b);
            }
            if ((UINT)t[i].Width == g_outW && (UINT)t[i].Height == g_outH) { t[i].Width = (FLOAT)g_fakeWv; t[i].Height = (FLOAT)g_fakeHv; }
        }
        o_RSSetViewports(c, n, t);
        return;
    }
    o_RSSetViewports(c, n, v);
}

static void STDMETHODCALLTYPE hkRSSetScissorRects(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r) {
    if (g_portraitOutFlag && !g_ovDrawing && r && n > 0 && n <= 16) {
        D3D11_RECT t[16];
        for (UINT i = 0; i < n; i++) {
            t[i] = r[i];
            if (g_scLogs < 8) {
                g_scLogs++;
                char b[128];
                wsprintfA(b, "th06nc-tate: scissor %ld,%ld,%ld,%ld", r[i].left, r[i].top, r[i].right, r[i].bottom);
                logLine(b);
            }
            // anything reaching the real window's right/bottom edge gets the fake buffer's edge
            if ((UINT)t[i].right == g_outW && (UINT)t[i].bottom == g_outH) { t[i].right = g_fakeWv; t[i].bottom = g_fakeHv; }
        }
        o_RSSetScissorRects(c, n, t);
        return;
    }
    o_RSSetScissorRects(c, n, r);
}

static void toggleMenu();
static pD3DCompile g_compile;
static ID3D11PixelShader* g_psOverlay;
static ID3D11BlendState*  g_bsPremul;

// ImGui's DX11 backend links against D3DCompile; forward it to the DLL we loaded at runtime.
extern "C" HRESULT WINAPI D3DCompile(const void* src, SIZE_T len, const char* name, const D3D_SHADER_MACRO* defs,
                                     ID3DInclude* inc, const char* entry, const char* target, UINT f1, UINT f2,
                                     ID3DBlob** code, ID3DBlob** err) {
    return g_compile ? g_compile(src, len, name, defs, inc, entry, target, f1, f2, code, err) : E_FAIL;
}

static void rebuildSampler() {
    if (!g_dev) return;
    SafeRelease(g_smp);
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = g_filter ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    g_dev->CreateSamplerState(&sd, &g_smp);
}

static const char kOverlayPs[] =
    "Texture2D tx:register(t0);SamplerState ss:register(s0);"
    "struct VO{float4 p:SV_Position;float2 t:TEXCOORD0;float2 l:TEXCOORD1;float2 f:TEXCOORD2;};"
    "float4 ps(VO i):SV_Target{return tx.Sample(ss,i.t);}";

static bool initRenderer(ID3D11Device* dev) {
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    if (!dc) dc = LoadLibraryA("d3dcompiler_43.dll");
    if (!dc) { logLine("th06nc-tate: no d3dcompiler found"); return false; }
    pD3DCompile compile = (pD3DCompile)(void*)GetProcAddress(dc, "D3DCompile");
    if (!compile) return false;
    g_compile = compile;

    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "tate", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vsb, &err))) {
        if (err) { logLine((const char*)err->GetBufferPointer()); err->Release(); }
        return false;
    }
    if (FAILED(compile(kShader, sizeof(kShader) - 1, "tate", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        if (err) { logLine((const char*)err->GetBufferPointer()); err->Release(); }
        vsb->Release();
        return false;
    }

    bool ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g_vs)) &&
              SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g_ps));
    if (ok) {
        D3D11_INPUT_ELEMENT_DESC ie[4] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32_FLOAT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ok = SUCCEEDED(dev->CreateInputLayout(ie, 4, vsb->GetBufferPointer(), vsb->GetBufferSize(), &g_il));
    }
    vsb->Release();
    psb->Release();
    if (!ok) return false;

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Vtx) * kMaxVerts;
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateBuffer(&bd, nullptr, &g_vb))) return false;

    g_dev = dev; // rebuildSampler uses it; AddRef happens below
    rebuildSampler();
    g_dev = nullptr;
    if (!g_smp) return false;
    {
        ID3DBlob* ob = nullptr;
        if (SUCCEEDED(compile(kOverlayPs, sizeof(kOverlayPs) - 1, "tate-ov", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &ob, nullptr))) {
            dev->CreatePixelShader(ob->GetBufferPointer(), ob->GetBufferSize(), nullptr, &g_psOverlay);
            ob->Release();
        }
        D3D11_BLEND_DESC pb = {};
        pb.RenderTarget[0].BlendEnable = TRUE;
        pb.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;            // overlay texture is premultiplied
        pb.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        pb.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        pb.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        pb.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
        pb.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        pb.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
        dev->CreateBlendState(&pb, &g_bsPremul);
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(dev->CreateRasterizerState(&rd, &g_rs))) return false;

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].BlendEnable = TRUE;
    bld.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bld.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bld.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bld.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    bld.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_BLUE;
    if (FAILED(dev->CreateBlendState(&bld, &g_bs))) return false;

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE;
    dsd.StencilEnable = FALSE;
    if (FAILED(dev->CreateDepthStencilState(&dsd, &g_dss))) return false;

    dev->GetImmediateContext(&g_ctx);
    g_dev = dev;
    g_dev->AddRef();
    {
        void* p;
        if ((p = patchVT(g_ctx, 44, (void*)hkRSSetViewports)))    o_RSSetViewports    = (PFN_RSSetViewports)p;
        if ((p = patchVT(g_ctx, 45, (void*)hkRSSetScissorRects))) o_RSSetScissorRects = (PFN_RSSetScissorRects)p;
    }
    loadBackground();
    {
        static bool first = true;
        if (first && g_openAtStartCfg) toggleMenu();
        first = false;
    }
    logLine("th06nc-tate: renderer ready");
    return true;
}

static void releaseCopy() {
    SafeRelease(g_copySrv);
    SafeRelease(g_copy);
    g_copyW = g_copyH = 0;
}

static void ensureStage(DXGI_FORMAT fmt) {
    if (g_stage[0] && g_stageFmt == fmt) return;
    for (int i = 0; i < 3; i++) { SafeRelease(g_stage[i]); g_stageFilled[i] = false; }
    D3D11_TEXTURE2D_DESC st = {};
    st.Width = 32; st.Height = 32; st.MipLevels = 1; st.ArraySize = 1;
    st.Format = fmt;
    st.SampleDesc.Count = 1;
    st.Usage = D3D11_USAGE_STAGING;
    st.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < 3; i++) g_dev->CreateTexture2D(&st, nullptr, &g_stage[i]);
    g_stageFmt = fmt;
}

static bool ensureCopy(const D3D11_TEXTURE2D_DESC& bbd) {
    if (g_copy && g_copyW == bbd.Width && g_copyH == bbd.Height && g_copyFmt == bbd.Format) return true;
    releaseCopy();
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = bbd.Width;
    d.Height = bbd.Height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = bbd.Format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_copy))) return false;
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = typedFormat(bbd.Format);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateShaderResourceView(g_copy, &sv, &g_copySrv))) { SafeRelease(g_copy); return false; }

    g_copyW = bbd.Width; g_copyH = bbd.Height; g_copyFmt = bbd.Format;
    char b[128];
    wsprintfA(b, "th06nc-tate: backbuffer %ux%u format %d", bbd.Width, bbd.Height, (int)bbd.Format);
    logLine(b);
    return true;
}

// ---------------------------------------------------------------- game picture area
// The game always draws a 16:9 picture. In a frame of another shape (e.g. 4:3) it letterboxes or
// pillarboxes it, centered. All source coordinates are measured inside that 16:9 area.
static float g_cx, g_cy, g_cw, g_ch;
static void contentRect() {
    float w = (float)g_srcW, h = (float)g_srcH;
    if (w <= 0 || h <= 0) { g_cx = g_cy = 0; g_cw = w; g_ch = h; return; }
    if (w * 9 > h * 16 + 8)      { g_ch = h; g_cw = h * 16 / 9; }   // wider than 16:9: pillarbox
    else if (w * 9 + 8 < h * 16) { g_cw = w; g_ch = w * 9 / 16; }   // taller than 16:9: letterbox
    else { g_cw = w; g_ch = h; }
    g_cx = (w - g_cw) * 0.5f;
    g_cy = (h - g_ch) * 0.5f;
}

// ---------------------------------------------------------------- gameplay detection
// The New Classic playfield has a gold frame. We read three short pixel strips across
// its left, right and top edges; all three gold = in game (includes pause/continue).
static const int kProbe = 30;

static bool isGold(int r, int g, int b) {
    return r >= 140 && r > g && g * 100 >= r * 68 && b * 100 <= r * 60;
}

static void readPixel(const unsigned char* row, int x, DXGI_FORMAT f, int* r, int* g, int* b) {
    const unsigned char* p = row + x * 4;
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        *b = p[0]; *g = p[1]; *r = p[2]; break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
        UINT v = *(const UINT*)p;
        *r = (v & 1023) >> 2; *g = ((v >> 10) & 1023) >> 2; *b = ((v >> 20) & 1023) >> 2; break;
    }
    default:
        *r = p[0]; *g = p[1]; *b = p[2]; break;
    }
}

static bool detectFormatOk(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return true;
    default: return false;
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void copyStrip(ID3D11Texture2D* dst, int dx, int dy, int cx, int cy, bool vertical) {
    D3D11_BOX box;
    if (vertical) {
        int y0 = clampi(cy - kProbe / 2, 0, (int)g_srcH - kProbe);
        box.left = clampi(cx, 0, g_srcW - 1); box.right = box.left + 1;
        box.top = y0; box.bottom = y0 + kProbe;
    } else {
        int x0 = clampi(cx - kProbe / 2, 0, (int)g_srcW - kProbe);
        box.left = x0; box.right = x0 + kProbe;
        box.top = clampi(cy, 0, g_srcH - 1); box.bottom = box.top + 1;
    }
    box.front = 0; box.back = 1;
    g_ctx->CopySubresourceRegion(dst, 0, dx, dy, 0, g_srcTex, 0, &box);
}

// returns -1 = no result yet, 0/1 = detected
static int detectGameplay() {
    if (!detectFormatOk(g_srcFmt) || g_srcW < 64 || g_srcH < 64) return -1;
    ensureStage(g_srcFmt);
    if (!g_stage[0]) return -1;
    contentRect();
    float s = g_cw / 2560.0f, t = g_ch / 1440.0f;
    int ox = (int)g_cx, oy = (int)g_cy;
    ID3D11Texture2D* st = g_stage[g_stageIdx];
    copyStrip(st, 0, 0,  ox + (int)(699 * s),  oy + (int)(720 * t), false); // left frame edge  -> row 0
    copyStrip(st, 0, 1,  ox + (int)(1867 * s), oy + (int)(720 * t), false); // right frame edge -> row 1
    copyStrip(st, 31, 2, ox + (int)(1280 * s), oy + (int)(40 * t),  true);  // top frame edge   -> column 31
    g_stageFilled[g_stageIdx] = true;

    int readIdx = (g_stageIdx + 1) % 3; // copied two frames ago
    g_stageIdx = readIdx;
    if (!g_stageFilled[readIdx]) return -1;

    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(g_stage[readIdx], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) return -1;
    const unsigned char* base = (const unsigned char*)m.pData;
    int r, g, b;
    bool left = false, right = false, top = false;
    for (int i = 0; i < kProbe; i++) {
        readPixel(base, i, g_srcFmt, &r, &g, &b);                 if (isGold(r, g, b)) left = true;
        readPixel(base + m.RowPitch, i, g_srcFmt, &r, &g, &b);    if (isGold(r, g, b)) right = true;
        readPixel(base + m.RowPitch * (2 + i), 31, g_srcFmt, &r, &g, &b); if (isGold(r, g, b)) top = true;
    }
    g_ctx->Unmap(g_stage[readIdx], 0);
    return (left && right && top) ? 1 : 0;
}

// ---------------------------------------------------------------- quad building
// Canvas = what the viewer sees on the rotated monitor. Map canvas pixel -> screen NDC.
// With ScreenShape set, the canvas is measured in "physical" units (the shape the display really shows)
// and squeezed to the output here, so it looks right after the display stretches the frame.
static float g_W, g_H;
static float g_vsx = 1, g_vsy = 1;   // virtual canvas -> real canvas scale

// Sets up the virtual canvas for this frame and returns its size.
static void canvasSize(float* cw, float* ch) {
    bool turned = (g_rotation & 1) != 0;
    float rw = turned ? g_H : g_W, rh = turned ? g_W : g_H;
    g_vsx = g_vsy = 1;
    if (g_screenShape && rw > 0 && rh > 0) {
        float a = g_screenShape == 1 ? 4.0f / 3.0f : 16.0f / 9.0f;   // long / short
        float want = rh > rw ? 1.0f / a : a;                         // physical width / height
        float vw = rh * want;
        g_vsx = rw / vw;
        rw = vw;
    }
    *cw = rw; *ch = rh;
}

static void toNdc(float u, float v, float* x, float* y) {
    u *= g_vsx; v *= g_vsy;
    float sx, sy;
    switch (g_rotation) {
    case 1:  sx = v;         sy = g_H - u;   break;
    case 2:  sx = g_W - u;   sy = g_H - v;   break;
    case 3:  sx = g_W - v;   sy = u;         break;
    default: sx = u;         sy = v;         break;
    }
    *x = sx / g_W * 2.0f - 1.0f;
    *y = 1.0f - sy / g_H * 2.0f;
}

static int pushQuad(Vtx* out, int n,
                    float s0, float t0, float s1, float t1,     // texture coords 0..1
                    float dx, float dy, float dw, float dh,     // dest, canvas pixels
                    float fx = 0, float fy = 0) {               // edge fade, canvas pixels
    if (n + 6 > kMaxVerts) return n;
    Vtx c[4];
    float ffx = (fx > 0 && dw > 0) ? fx / dw : 0, ffy = (fy > 0 && dh > 0) ? fy / dh : 0;
    if (ffx > 0.5f) ffx = 0.5f;
    if (ffy > 0.5f) ffy = 0.5f;
    for (int i = 0; i < 4; i++) { c[i].lx = (float)(i & 1); c[i].ly = (float)(i >> 1); c[i].fx = ffx; c[i].fy = ffy; }
    toNdc(dx,      dy,      &c[0].x, &c[0].y); c[0].u = s0; c[0].v = t0;
    toNdc(dx + dw, dy,      &c[1].x, &c[1].y); c[1].u = s1; c[1].v = t0;
    toNdc(dx,      dy + dh, &c[2].x, &c[2].y); c[2].u = s0; c[2].v = t1;
    toNdc(dx + dw, dy + dh, &c[3].x, &c[3].y); c[3].u = s1; c[3].v = t1;
    out[n++] = c[0]; out[n++] = c[1]; out[n++] = c[2];
    out[n++] = c[2]; out[n++] = c[1]; out[n++] = c[3];
    return n;
}

// Returns vertex count; *bgVerts = how many leading verts sample the background image.
static int buildQuads(Vtx* v, bool layout, int* bgVerts) {
    float cw, ch;
    canvasSize(&cw, &ch);
    bool portrait = ch > cw;
    int n = 0;
    *bgVerts = 0;
    contentRect();
    float tx0 = g_cx / g_srcW, ty0 = g_cy / g_srcH;          // 16:9 picture area in texture units
    float tsx = g_cw / g_srcW, tsy = g_ch / g_srcH;
    EnterCriticalSection(&g_lock);
    if (layout && portrait && g_numRects > 0) {
        // fit the 3:4 layout canvas into the screen, centered
        float k = cw / g_canW;
        if (ch / g_canH < k) k = ch / g_canH;
        float ox = (cw - g_canW * k) * 0.5f, oy = (ch - g_canH * k) * 0.5f;
        if (g_bgSrv) {
            n = pushQuad(v, n, 0, 0, 1, 1, ox, oy, g_canW * k, g_canH * k);
            *bgVerts = n;
        }
        for (int i = 0; i < g_numRects; i++) {
            const Rect8& r = g_rects[i];
            n = pushQuad(v, n,
                         tx0 + tsx * r.sx / g_refW, ty0 + tsy * r.sy / g_refH,
                         tx0 + tsx * (r.sx + r.sw) / g_refW, ty0 + tsy * (r.sy + r.sh) / g_refH,
                         ox + r.dx * k, oy + r.dy * k, r.dw * k, r.dh * k,
                         i == 0 ? 0 : g_featherX * k, i == 0 ? 0 : g_featherY * k);
        }
    } else {
        // whole frame, rotated and fitted (menus)
        // whole picture (menus), rotated and fitted; letterbox bars of a non-16:9 frame are dropped
        float s = cw / g_cw;
        if (ch / g_ch < s) s = ch / g_ch;
        float w = g_cw * s, h = g_ch * s;
        n = pushQuad(v, n, tx0, ty0, tx0 + tsx, ty0 + tsy, (cw - w) * 0.5f, (ch - h) * 0.5f, w, h);
    }
    LeaveCriticalSection(&g_lock);
    return n;
}

// ---------------------------------------------------------------- input + mode
static bool g_keyPrev[5];
static bool g_playing;      // debounced detection result
static int  g_streak;
static volatile bool g_layoutNow;

static bool gameFocused() {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void pollKeys() {
    static const int keys[5] = { VK_LEFT, VK_RIGHT, VK_UP, VK_DOWN, '0' };
    bool alt = gameFocused() && (GetAsyncKeyState(VK_MENU) & 0x8000);
    for (int i = 0; i < 5; i++) {
        bool down = alt && (GetAsyncKeyState(keys[i]) & 0x8000);
        if (down && !g_keyPrev[i]) {
            if (i == 0) { g_rotation = (g_rotation + 3) & 3; saveRotation(); } // clockwise
            if (i == 1) { g_rotation = (g_rotation + 1) & 3; saveRotation(); } // counterclockwise
            if (i == 2) g_force = 1;
            if (i == 3) g_force = 0;
            if (i == 4) toggleMenu();
        }
        g_keyPrev[i] = down;
    }
}

static bool layoutActive(int detected) {
    if (!g_autoDetect || detected < 0) {
        if (!g_autoDetect) return g_force >= 0 ? g_force == 1 : g_defaultLayout != 0;
        return g_force >= 0 ? g_force == 1 : g_playing; // no new sample this frame
    }
    bool now = detected == 1;
    if (now != g_playing) {
        if (++g_streak >= 8) { // debounce
            g_playing = now;
            g_streak = 0;
            g_force = -1;      // detection changed, drop manual override
            if (g_debug) logLine(now ? "th06nc-tate: gameplay detected" : "th06nc-tate: left gameplay");
        }
    } else {
        g_streak = 0;
    }
    return g_force >= 0 ? g_force == 1 : g_playing;
}

// ---------------------------------------------------------------- Alt+0 in-game menu (Dear ImGui, THRotator layout)
// Drawn into a texture the size of what the viewer sees (upright), then rotated onto the screen with the frame.
static bool g_ovReady;
static ID3D11Texture2D*          g_ovTex;
static ID3D11RenderTargetView*   g_ovRtv;
static ID3D11ShaderResourceView* g_ovSrv;
static UINT g_ovW, g_ovH;
static LARGE_INTEGER g_ovFreq, g_ovLast;
static float g_ovScale = 1;
static int  g_ovSel = 1;                  // selected "other region"
static char g_ovBgEdit[MAX_PATH];
static RECT realClient(HWND h);

extern "C" void tate_assert_fail(const char* expr, const char* file, int line) {
    static int n;
    if (n++ >= 20) return;
    char b[512];
    wsprintfA(b, "th06nc-tate: ImGui assert %s (%s:%d)", expr, file, line);
    logLine(b);
}

// Window messages may come on another thread than Present, so input is queued and fed to ImGui per frame.
struct OvEvent { int type; float x, y; int v; }; // 0 pos, 1 button, 2 wheel, 3 char, 4 key, 5 mods
static OvEvent g_ovQueue[256];
static int g_ovCount;
static void ovPush(int type, float x, float y, int v) {
    EnterCriticalSection(&g_lock);
    if (g_ovCount < 256) { OvEvent e = { type, x, y, v }; g_ovQueue[g_ovCount++] = e; }
    LeaveCriticalSection(&g_lock);
}
static void ovDrain() {
    ImGuiIO& io = ImGui::GetIO();
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_ovCount; i++) {
        const OvEvent& e = g_ovQueue[i];
        switch (e.type) {
        case 0: io.AddMousePosEvent(e.x, e.y); break;
        case 1: io.AddMouseButtonEvent((int)e.x, e.v != 0); break;
        case 2: io.AddMouseWheelEvent(0, e.y); break;
        case 3: io.AddInputCharacter((unsigned)e.v); break;
        case 4: io.AddKeyEvent((ImGuiKey)(int)e.x, e.v != 0); break;
        case 5: io.AddKeyEvent(ImGuiMod_Ctrl, (e.v & 1) != 0); io.AddKeyEvent(ImGuiMod_Shift, (e.v & 2) != 0); break;
        }
    }
    g_ovCount = 0;
    LeaveCriticalSection(&g_lock);
}

static void overlayInit() {
    if (g_ovReady || !g_dev) return;
    logLine("th06nc-tate: menu init");
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.Fonts->AddFontDefaultVector();
    ImGui::StyleColorsDark();
    ImGui_ImplDX11_Init(g_dev, g_ctx);
    QueryPerformanceFrequency(&g_ovFreq);
    QueryPerformanceCounter(&g_ovLast);
    lstrcpyA(g_ovBgEdit, g_bgName);
    g_ovReady = true;
    logLine("th06nc-tate: menu ready");
}

static bool overlayEnsureTarget(UINT w, UINT h) {
    if (g_ovTex && g_ovW == w && g_ovH == h) return true;
    SafeRelease(g_ovSrv); SafeRelease(g_ovRtv); SafeRelease(g_ovTex);
    D3D11_TEXTURE2D_DESC d = {};
    d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_ovTex))) return false;
    g_dev->CreateRenderTargetView(g_ovTex, nullptr, &g_ovRtv);
    g_dev->CreateShaderResourceView(g_ovTex, nullptr, &g_ovSrv);
    g_ovW = w; g_ovH = h;
    // scale with the screen: 1.0 at a 1080-pixel short side
    float s = (float)(w < h ? w : h) / 1080.0f;
    if (s < 0.75f) s = 0.75f;
    ImGuiStyle& st = ImGui::GetStyle();
    st = ImGuiStyle();
    ImGui::StyleColorsDark(&st);
    st.ScaleAllSizes(s);
    st.FontSizeBase = 18.0f;
    st.FontScaleMain = s;
    g_ovScale = s;
    return g_ovRtv && g_ovSrv;
}

// window client pixel -> pixel on the upright (viewer) canvas; inverse of toNdc
static bool overlayMouse(HWND hwnd, LPARAM lp, float* cx, float* cy) {
    RECT rc = realClient(hwnd);
    if (rc.right <= 0 || rc.bottom <= 0 || g_W <= 0 || g_H <= 0) return false;
    float sx = (float)(short)LOWORD(lp) * g_W / rc.right;
    float sy = (float)(short)HIWORD(lp) * g_H / rc.bottom;
    switch (g_rotation) {
    case 1:  *cx = g_H - sy; *cy = sx;       break;
    case 2:  *cx = g_W - sx; *cy = g_H - sy; break;
    case 3:  *cx = sy;       *cy = g_W - sx; break;
    default: *cx = sx;       *cy = sy;       break;
    }
    *cx /= g_vsx; *cy /= g_vsy;
    return true;
}

static ImGuiKey vkToKey(WPARAM vk) {
    switch (vk) {
    case VK_TAB: return ImGuiKey_Tab;           case VK_LEFT: return ImGuiKey_LeftArrow;
    case VK_RIGHT: return ImGuiKey_RightArrow;  case VK_UP: return ImGuiKey_UpArrow;
    case VK_DOWN: return ImGuiKey_DownArrow;    case VK_HOME: return ImGuiKey_Home;
    case VK_END: return ImGuiKey_End;           case VK_DELETE: return ImGuiKey_Delete;
    case VK_BACK: return ImGuiKey_Backspace;    case VK_RETURN: return ImGuiKey_Enter;
    case VK_ESCAPE: return ImGuiKey_Escape;     case 'A': return ImGuiKey_A;
    case 'C': return ImGuiKey_C;                case 'V': return ImGuiKey_V;
    case 'X': return ImGuiKey_X;                case 'Z': return ImGuiKey_Z;
    default: return ImGuiKey_None;
    }
}

// returns true when the menu used the message (the game doesn't get it)
static bool overlayInput(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g_ovVisible || !g_ovReady) return false;
    ImGuiIO& io = ImGui::GetIO(); // only flags read here; events go through the queue
    float cx, cy;
    switch (msg) {
    case WM_MOUSEMOVE:
        if (overlayMouse(h, lp, &cx, &cy)) ovPush(0, cx, cy, 0);
        return io.WantCaptureMouse;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        if (overlayMouse(h, lp, &cx, &cy)) ovPush(0, cx, cy, 0);
        int b = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? 0 : 1;
        ovPush(1, (float)b, 0, msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN);
        return io.WantCaptureMouse;
    }
    case WM_MOUSEWHEEL:
        ovPush(2, 0, (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, 0);
        return io.WantCaptureMouse;
    case WM_CHAR:
        if (io.WantTextInput && wp >= 32 && wp < 0x10000) { ovPush(3, 0, 0, (int)wp); return true; }
        return false;
    case WM_KEYDOWN: case WM_KEYUP: {
        int mods = ((GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0) | ((GetKeyState(VK_SHIFT) & 0x8000) ? 2 : 0);
        ovPush(5, 0, 0, mods);
        ImGuiKey k = vkToKey(wp);
        if (k != ImGuiKey_None) ovPush(4, (float)(int)k, 0, msg == WM_KEYDOWN);
        return io.WantTextInput;
    }
    }
    return false;
}

static bool editRect(Rect8& r) {
    bool c = false;
    if (ImGui::InputInt4("Source x, y, w, h", &r.sx)) { c = true; if (r.sw < 1) r.sw = 1; if (r.sh < 1) r.sh = 1; }
    if (ImGui::InputInt4("Destination x, y, w, h", &r.dx)) { c = true; if (r.dw < 1) r.dw = 1; if (r.dh < 1) r.dh = 1; }
    return c;
}

static void overlayBuildUi() {
    ImGui::SetNextWindowPos(ImVec2(30 * g_ovScale, 30 * g_ovScale), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(640 * g_ovScale, 0), ImGuiCond_FirstUseEver);
    bool open = true;
    if (ImGui::Begin("th06nc-tate", &open)) {
        static const char* rots[4] = { "0", "90", "180", "270" };
        for (int i = 0; i < 4; i++) {
            if (i) ImGui::SameLine();
            if (ImGui::RadioButton(rots[i], g_rotation == i)) { g_rotation = i; saveRotation(); }
        }
        bool b = g_openAtStartCfg != 0;
        if (ImGui::Checkbox("Open this menu at startup", &b)) g_openAtStartCfg = b;
        b = g_force == 1;
        if (ImGui::Checkbox("Force HUD layout (not saved)", &b)) g_force = b ? 1 : -1;

        EnterCriticalSection(&g_lock);
        if (ImGui::CollapsingHeader("Gameplay detection")) {
            b = g_autoDetect != 0;
            if (ImGui::Checkbox("Auto detect (gold playfield frame)", &b)) g_autoDetect = b;
            ImGui::Text("In gameplay: %s      Showing: %s", g_playing ? "yes" : "no",
                        ((g_rotation & 1) || g_portraitOut) ? (g_layoutNow ? "HUD layout" : "whole screen") : "horizontal");
            b = g_defaultLayout != 0;
            if (ImGui::Checkbox("HUD layout when auto detect is off", &b)) g_defaultLayout = b;
        }
        if (ImGui::CollapsingHeader("Main screen region (playfield)") && g_numRects > 0) {
            ImGui::PushID("pf");
            editRect(g_rects[0]);
            ImGui::PopID();
        }
        if (ImGui::CollapsingHeader("Pixel interpolation")) {
            if (ImGui::RadioButton("None", g_filter == 0)) { g_filter = 0; g_samplerDirty = true; }
            ImGui::SameLine();
            if (ImGui::RadioButton("Bilinear", g_filter != 0)) { g_filter = 1; g_samplerDirty = true; }
        }
        if (ImGui::CollapsingHeader("Other regions")) {
            if (g_ovSel >= g_numRects) g_ovSel = g_numRects - 1;
            if (ImGui::BeginListBox("##regions", ImVec2(-120 * g_ovScale, 180 * g_ovScale))) {
                for (int i = 1; i < g_numRects; i++) {
                    const Rect8& r = g_rects[i];
                    char t[128];
                    wsprintfA(t, "Region %d:  %d,%d %dx%d -> %d,%d %dx%d", i, r.sx, r.sy, r.sw, r.sh, r.dx, r.dy, r.dw, r.dh);
                    if (ImGui::Selectable(t, g_ovSel == i)) g_ovSel = i;
                }
                ImGui::EndListBox();
            }
            ImGui::SameLine();
            ImGui::BeginGroup();
            float bw = 100 * g_ovScale;
            if (ImGui::Button("Add", ImVec2(bw, 0)) && g_numRects < kMaxRects) {
                Rect8 r = { 1950, 158, 560, 62, 8, 4, 506, 56 };
                if (g_ovSel >= 1 && g_ovSel < g_numRects) r = g_rects[g_ovSel];
                g_rects[g_numRects] = r;
                g_ovSel = g_numRects++;
            }
            if (ImGui::Button("Delete", ImVec2(bw, 0)) && g_ovSel >= 1 && g_ovSel < g_numRects) {
                for (int i = g_ovSel; i < g_numRects - 1; i++) g_rects[i] = g_rects[i + 1];
                g_numRects--;
                if (g_ovSel >= g_numRects) g_ovSel = g_numRects - 1;
            }
            if (ImGui::Button("Up", ImVec2(bw, 0)) && g_ovSel > 1 && g_ovSel < g_numRects) {
                Rect8 t = g_rects[g_ovSel]; g_rects[g_ovSel] = g_rects[g_ovSel - 1]; g_rects[g_ovSel - 1] = t; g_ovSel--;
            }
            if (ImGui::Button("Down", ImVec2(bw, 0)) && g_ovSel >= 1 && g_ovSel < g_numRects - 1) {
                Rect8 t = g_rects[g_ovSel]; g_rects[g_ovSel] = g_rects[g_ovSel + 1]; g_rects[g_ovSel + 1] = t; g_ovSel++;
            }
            ImGui::EndGroup();
            if (g_ovSel >= 1 && g_ovSel < g_numRects) {
                ImGui::PushID("rg");
                editRect(g_rects[g_ovSel]);
                ImGui::PopID();
            }
            int f[2] = { g_featherX, g_featherY };
            if (ImGui::InputInt2("Edge fade x, y", f)) { g_featherX = f[0] < 0 ? 0 : f[0]; g_featherY = f[1] < 0 ? 0 : f[1]; }
        }
        if (ImGui::CollapsingHeader("Display")) {
            ImGui::InputText("Background image (BMP)", g_ovBgEdit, sizeof(g_ovBgEdit));
            ImGui::SameLine();
            if (ImGui::Button("Apply")) { lstrcpyA(g_bgName, g_ovBgEdit); g_bgDirty = true; }
            static const char* pd[3] = { "Auto", "On", "Off" };
            int cur = g_portraitDesktop < 0 ? 0 : (g_portraitDesktop ? 1 : 2);
            if (ImGui::Combo("Non-16:9 desktop fix (next launch)", &cur, pd, 3)) g_portraitDesktop = cur == 0 ? -1 : (cur == 1 ? 1 : 0);
            static const char* ss[3] = { "Same as resolution", "4:3 (stretched by display)", "16:9 (stretched by display)" };
            ImGui::Combo("Screen shape", &g_screenShape, ss, 3);
            ImGui::TextDisabled("For CRTs or screens that stretch the picture to fill. Normally: Same as resolution.");
            int ref[2] = { g_refW, g_refH }, can[2] = { g_canW, g_canH };
            if (ImGui::InputInt2("Source reference w, h", ref) && ref[0] > 0 && ref[1] > 0) { g_refW = ref[0]; g_refH = ref[1]; }
            if (ImGui::InputInt2("Rotated layout size w, h", can) && can[0] > 0 && can[1] > 0) { g_canW = can[0]; g_canH = can[1]; }
            ImGui::TextDisabled("Region destinations are measured in this size. Default 1440x1920 (3:4).");
            b = g_debug != 0;
            if (ImGui::Checkbox("Debug log", &b)) g_debug = b;
        }
        LeaveCriticalSection(&g_lock);

        if (ImGui::Button("Hide")) open = false;
        ImGui::SameLine();
        if (ImGui::Button("Reload")) { loadConfig(); g_samplerDirty = true; g_bgDirty = true; lstrcpyA(g_ovBgEdit, g_bgName); }
        ImGui::SameLine();
        if (ImGui::Button("Save")) { lstrcpyA(g_bgName, g_ovBgEdit); saveAll(); }
        ImGui::SameLine();
        if (ImGui::Button("Version info")) ImGui::OpenPopup("version");
        ImGui::SameLine();
        if (ImGui::Button("Reset to defaults")) ImGui::OpenPopup("reset");
        if (ImGui::BeginPopup("reset")) {
            ImGui::Text("Reset every setting except rotation? (Save afterwards to keep it.)");
            if (ImGui::Button("Reset")) { resetDefaults(); lstrcpyA(g_ovBgEdit, g_bgName); g_ovSel = 1; ImGui::CloseCurrentPopup(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("version")) {
            ImGui::Text("th06nc-tate: THRotator-style tate mod for Touhou 6 New Classic");
            ImGui::Text("Alt+Left / Alt+Right rotate, Alt+Up / Alt+Down force layout, Alt+0 this menu");
            ImGui::EndPopup();
        }
    }
    ImGui::End();
    if (!open) g_ovVisible = false;
}

// Renders the menu into its texture. Called from onFrame with the game's state already saved.
static bool overlayRender(float cw, float ch) {
    if (!g_ovVisible) return false;
    overlayInit();
    if (!g_ovReady || !overlayEnsureTarget((UINT)cw, (UINT)ch)) return false;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = g_ovFreq.QuadPart ? (float)(now.QuadPart - g_ovLast.QuadPart) / g_ovFreq.QuadPart : 1.0f / 60;
    if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60;
    g_ovLast = now;
    io.DisplaySize = ImVec2(cw, ch);
    io.MouseDrawCursor = true;

    static int step;
    if (step == 0) { logLine("th06nc-tate: menu first frame"); step = 1; }
    ovDrain();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    overlayBuildUi();
    ImGui::Render();

    const FLOAT clear[4] = { 0, 0, 0, 0 };
    g_ctx->OMSetRenderTargets(1, &g_ovRtv, nullptr);
    g_ctx->ClearRenderTargetView(g_ovRtv, clear);
    g_ovDrawing = true;
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_ovDrawing = false;
    if (step == 1) { logLine("th06nc-tate: menu drawn"); step = 2; }
    return true;
}

static void toggleMenu() { g_ovVisible = !g_ovVisible; }

// ---------------------------------------------------------------- the frame hook
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetBuffer)(IDXGISwapChain*, UINT, REFIID, void**);
static PFN_GetBuffer o_GetBuffer;

static bool ensureRenderer(IDXGISwapChain* sc) {
    if (g_dev) return true;
    if (g_initFailed) return false;
    ID3D11Device* dev = nullptr;
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) { g_initFailed = true; return false; }
    bool ok = initRenderer(dev);
    dev->Release();
    if (!ok) { g_initFailed = true; logLine("th06nc-tate: renderer init failed"); }
    return ok;
}

static void onFrame(IDXGISwapChain* sc) {
    pollKeys();
    bool portraitOut = g_portraitOut && g_fake;
    if ((!portraitOut && g_rotation == 0 && !g_ovVisible) || g_initFailed) return;
    if (!ensureRenderer(sc)) return;
    if (g_samplerDirty) { g_samplerDirty = false; rebuildSampler(); }
    if (g_bgDirty) { g_bgDirty = false; SafeRelease(g_bgSrv); loadBackground(); }

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(o_GetBuffer(sc, 0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC bbd;
    bb->GetDesc(&bbd);
    if (bbd.SampleDesc.Count != 1) { bb->Release(); return; }

    ID3D11RenderTargetView* rtv = nullptr;
    D3D11_RENDER_TARGET_VIEW_DESC rvd = {};
    rvd.Format = typedFormat(bbd.Format);
    rvd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    if (FAILED(g_dev->CreateRenderTargetView(bb, &rvd, &rtv))) { bb->Release(); return; }

    g_W = (float)bbd.Width;   // output size
    g_H = (float)bbd.Height;
    ID3D11DeviceContext* c = g_ctx;

    ID3D11ShaderResourceView* srcSrv;
    if (portraitOut) {
        // the game drew into our fake landscape buffer
        g_srcTex = g_fake; g_srcW = g_fakeW; g_srcH = g_fakeH; g_srcFmt = bbd.Format;
        srcSrv = g_fakeSrv;
    } else {
        if (!ensureCopy(bbd)) { rtv->Release(); bb->Release(); return; }
        c->CopyResource(g_copy, bb);
        g_srcTex = g_copy; g_srcW = g_copyW; g_srcH = g_copyH; g_srcFmt = g_copyFmt;
        srcSrv = g_copySrv;
    }

    int detected = g_autoDetect ? detectGameplay() : -1;
    bool layout = layoutActive(detected);
    g_layoutNow = layout;

    static Vtx verts[kMaxVerts];
    int bgVerts = 0;
    int nv = buildQuads(verts, layout, &bgVerts);
    float cw, ch;
    canvasSize(&cw, &ch);
    int ovVerts = 0;
    if (g_ovVisible && g_psOverlay && g_bsPremul) ovVerts = pushQuad(verts, nv, 0, 0, 1, 1, 0, 0, cw, ch) - nv;

    // ---- save state
    ID3D11InputLayout* sIl; c->IAGetInputLayout(&sIl);
    D3D11_PRIMITIVE_TOPOLOGY sTopo; c->IAGetPrimitiveTopology(&sTopo);
    ID3D11Buffer* sVb; UINT sStride, sOff; c->IAGetVertexBuffers(0, 1, &sVb, &sStride, &sOff);
    ID3D11VertexShader* sVs; UINT nci = 0; c->VSGetShader(&sVs, nullptr, &nci);
    ID3D11PixelShader* sPs; nci = 0; c->PSGetShader(&sPs, nullptr, &nci);
    ID3D11GeometryShader* sGs; nci = 0; c->GSGetShader(&sGs, nullptr, &nci);
    ID3D11HullShader* sHs; nci = 0; c->HSGetShader(&sHs, nullptr, &nci);
    ID3D11DomainShader* sDs; nci = 0; c->DSGetShader(&sDs, nullptr, &nci);
    ID3D11ShaderResourceView* sSrv; c->PSGetShaderResources(0, 1, &sSrv);
    ID3D11SamplerState* sSmp; c->PSGetSamplers(0, 1, &sSmp);
    ID3D11RasterizerState* sRs; c->RSGetState(&sRs);
    D3D11_VIEWPORT sVp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE];
    UINT nVp = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE; c->RSGetViewports(&nVp, sVp);
    ID3D11RenderTargetView* sRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]; ID3D11DepthStencilView* sDsv;
    c->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, sRtv, &sDsv);
    ID3D11BlendState* sBs; FLOAT sBf[4]; UINT sMask; c->OMGetBlendState(&sBs, sBf, &sMask);
    ID3D11DepthStencilState* sDss; UINT sRef; c->OMGetDepthStencilState(&sDss, &sRef);

    // ---- draw
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(c->Map(g_vb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, verts, sizeof(Vtx) * (nv + ovVerts));
        c->Unmap(g_vb, 0);

        const FLOAT black[4] = { 0, 0, 0, 1 };
        c->ClearRenderTargetView(rtv, black);
        UINT stride = sizeof(Vtx), off = 0;
        c->IASetInputLayout(g_il);
        c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        c->IASetVertexBuffers(0, 1, &g_vb, &stride, &off);
        c->VSSetShader(g_vs, nullptr, 0);
        c->GSSetShader(nullptr, nullptr, 0);
        c->HSSetShader(nullptr, nullptr, 0);
        c->DSSetShader(nullptr, nullptr, 0);
        c->PSSetShader(g_ps, nullptr, 0);
        c->PSSetSamplers(0, 1, &g_smp);
        c->RSSetState(g_rs);
        D3D11_VIEWPORT vp = { 0, 0, g_W, g_H, 0, 1 };
        o_RSSetViewports(c, 1, &vp);
        c->OMSetRenderTargets(1, &rtv, nullptr);
        c->OMSetBlendState(g_bs, nullptr, 0xffffffff);
        c->OMSetDepthStencilState(g_dss, 0);
        if (bgVerts > 0) {
            c->PSSetShaderResources(0, 1, &g_bgSrv);
            c->Draw(bgVerts, 0);
        }
        c->PSSetShaderResources(0, 1, &srcSrv);
        c->Draw(nv - bgVerts, bgVerts);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        c->PSSetShaderResources(0, 1, &nullSrv);

        // Alt+0 menu: rendered upright into its own texture, then rotated onto the screen like the frame
        if (ovVerts > 0 && overlayRender(cw, ch)) {
            c->OMSetRenderTargets(1, &rtv, nullptr);
            c->IASetInputLayout(g_il);
            c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            c->IASetVertexBuffers(0, 1, &g_vb, &stride, &off);
            c->VSSetShader(g_vs, nullptr, 0);
            c->PSSetShader(g_psOverlay, nullptr, 0);
            c->PSSetShaderResources(0, 1, &g_ovSrv);
            c->PSSetSamplers(0, 1, &g_smp);
            c->RSSetState(g_rs);
            c->OMSetBlendState(g_bsPremul, nullptr, 0xffffffff);
            c->OMSetDepthStencilState(g_dss, 0);
            o_RSSetViewports(c, 1, &vp);
            c->Draw(ovVerts, nv);
            c->PSSetShaderResources(0, 1, &nullSrv);
        }

    }

    // ---- restore state
    c->IASetInputLayout(sIl);
    c->IASetPrimitiveTopology(sTopo);
    c->IASetVertexBuffers(0, 1, &sVb, &sStride, &sOff);
    c->VSSetShader(sVs, nullptr, 0);
    c->PSSetShader(sPs, nullptr, 0);
    c->GSSetShader(sGs, nullptr, 0);
    c->HSSetShader(sHs, nullptr, 0);
    c->DSSetShader(sDs, nullptr, 0);
    c->PSSetShaderResources(0, 1, &sSrv);
    c->PSSetSamplers(0, 1, &sSmp);
    c->RSSetState(sRs);
    o_RSSetViewports(c, nVp, sVp);
    c->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, sRtv, sDsv);
    c->OMSetBlendState(sBs, sBf, sMask);
    c->OMSetDepthStencilState(sDss, sRef);

    SafeRelease(sIl); SafeRelease(sVb); SafeRelease(sVs); SafeRelease(sPs);
    SafeRelease(sGs); SafeRelease(sHs); SafeRelease(sDs); SafeRelease(sSrv);
    SafeRelease(sSmp); SafeRelease(sRs); SafeRelease(sDsv); SafeRelease(sBs); SafeRelease(sDss);
    for (int i = 0; i < D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT; i++) SafeRelease(sRtv[i]);

    rtv->Release();
    bb->Release();
}

// ---------------------------------------------------------------- swapchain hooks
typedef HRESULT (STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_Present       o_Present;
static PFN_Present1      o_Present1;
static PFN_ResizeBuffers o_ResizeBuffers;
static bool g_inPresent;

// ---- overlays above us (Special K, ReShade...) draw their UI before calling down into Present.
// We learn the spot in their code that grabs the backbuffer right before Present each frame, and from
// then on rotate the frame at that moment, so their UI lands on top of our output instead of being rotated.
struct SiteLearn { void* cand; void* last; int streak; void* learned; const char* name; };
static SiteLearn g_slBuf = { 0, 0, 0, 0, "GetBuffer" };
static SiteLearn g_slDesc = { 0, 0, 0, 0, "GetDesc" };
static SiteLearn g_slDesc1 = { 0, 0, 0, 0, "GetDesc1" };
static bool  g_frameDone;        // frame already rotated this frame (early, for the overlay)
static BYTE*   g_selfBase;
static HMODULE g_modCache[16];
static bool    g_modIsOverlay[16];
static int     g_modCount;

static bool isOverlayModule(void* addr) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) || !m)
        return false; // unpacked game code
    for (int i = 0; i < g_modCount; i++) if (g_modCache[i] == m) return g_modIsOverlay[i];
    char path[MAX_PATH] = "";
    GetModuleFileNameA(m, path, MAX_PATH);
    CharLowerA(path);
    bool sys = false;
    // system modules (DXVK, Wine) live in system32 / syswow64
    for (char* p = path; *p; p++) {
        const char* a = "\\system32\\";
        const char* b = "\\syswow64\\";
        int i = 0; while (a[i] && p[i] == a[i]) i++; if (!a[i]) { sys = true; break; }
        i = 0; while (b[i] && p[i] == b[i]) i++; if (!b[i]) { sys = true; break; }
    }
    bool overlay = !sys && m != GetModuleHandleA(nullptr) && (BYTE*)m != g_selfBase;
    if (g_modCount < 16) { g_modCache[g_modCount] = m; g_modIsOverlay[g_modCount] = overlay; g_modCount++; }
    if (overlay) {
        char b[MAX_PATH + 64];
        wsprintfA(b, "th06nc-tate: backbuffer requested by %s", path);
        logLine(b);
    }
    return overlay;
}

static void learnStep(SiteLearn& sl) {
    if (sl.cand && sl.cand == sl.last) sl.streak++;
    else sl.streak = sl.cand ? 1 : 0;
    sl.last = sl.cand;
    sl.cand = nullptr;
    if (!sl.learned && sl.streak >= 3) {
        sl.learned = sl.last;
        char b[96];
        wsprintfA(b, "th06nc-tate: overlay %s found, rotating before it draws", sl.name);
        logLine(b);
    }
}

// called at Present: learn the overlay's per-frame call sites (the last ones it makes before Present)
static void overlayLearn() {
    learnStep(g_slBuf);
    learnStep(g_slDesc);
    learnStep(g_slDesc1);
}

// true if this call comes from the overlay's own per-frame code: rotate the frame now and tell it the truth
static bool overlayCall(SiteLearn& sl, void* ret, IDXGISwapChain* sc) {
    return false; // ordering with overlays above us (Special K) is off: not reliable
    if (sl.learned && ret == sl.learned) {
        if (!g_frameDone && !g_inPresent) { onFrame(sc); g_frameDone = true; }
        return true;
    }
    if (!g_inPresent && isOverlayModule(ret)) sl.cand = ret;
    return false;
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (!g_inPresent && !(flags & DXGI_PRESENT_TEST)) {
        if (!g_frameDone) onFrame(sc);
        g_frameDone = false;
        overlayLearn();
    }
    g_inPresent = true;
    HRESULT hr = o_Present(sc, sync, flags);
    g_inPresent = false;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    if (!g_inPresent && !(flags & DXGI_PRESENT_TEST)) {
        if (!g_frameDone) onFrame(sc);
        g_frameDone = false;
        overlayLearn();
    }
    g_inPresent = true;
    HRESULT hr = o_Present1(sc, sync, flags, p);
    g_inPresent = false;
    return hr;
}

typedef HRESULT (STDMETHODCALLTYPE* PFN_GetDesc)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetDesc1)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
static PFN_GetDesc  o_GetDesc;
static PFN_GetDesc1 o_GetDesc1;

// ---- portrait desktop: make the game believe its window is landscape
// The game sizes its picture from the window, so GetClientRect (called from the game exe)
// and WM_SIZE report width and height swapped while the desktop is portrait.
static HWND    g_gameHwnd;
static WNDPROC g_origWndProc;
static bool    g_wndUnicode;
static BYTE*   g_exeLo;
static BYTE*   g_exeHi;
static int     g_clientLogs;
static bool    g_fakeFs;        // game asked for exclusive fullscreen; we run it as a monitor-sized window
static BOOL    g_fsState;
static RECT    g_monRect;

static RECT realClient(HWND h) {
    RECT r = { 0, 0, 0, 0 };
    WINDOWINFO wi = {};
    wi.cbSize = sizeof(wi);
    if (h && GetWindowInfo(h, &wi)) {
        r.right = wi.rcClient.right - wi.rcClient.left;
        r.bottom = wi.rcClient.bottom - wi.rcClient.top;
    }
    return r;
}

static BOOL WINAPI hkGetClientRect(HWND h, LPRECT out) {
    BYTE* ret = (BYTE*)__builtin_return_address(0);
    WINDOWINFO wi = {};
    wi.cbSize = sizeof(wi);
    if (!out || !GetWindowInfo(h, &wi)) { SetLastError(ERROR_INVALID_WINDOW_HANDLE); return FALSE; }
    out->left = 0; out->top = 0;
    out->right = wi.rcClient.right - wi.rcClient.left;
    out->bottom = wi.rcClient.bottom - wi.rcClient.top;
    // Lie only to the game: callers in the exe image or in unpacked code outside any module.
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)ret, &mod);
    bool fromGame = !mod || (ret >= g_exeLo && ret < g_exeHi);
    if (g_clientLogs < 12 && h == g_gameHwnd) {
        g_clientLogs++;
        char name[MAX_PATH] = "(no module)";
        if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
        char b[MAX_PATH + 80];
        wsprintfA(b, "th06nc-tate: GetClientRect caller %s lie=%d", name, (int)fromGame);
        logLine(b);
    }
    if (fromGame && h == g_gameHwnd && g_portraitOut && g_fakeWv) {
        out->right = g_fakeWv; out->bottom = g_fakeHv;  // the game sees its own 16:9 size
    }
    return TRUE;
}

static LRESULT CALLBACK hkWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_WINDOWPOSCHANGING && g_fakeFs && lp) {
        // only undo landscape resizes (the game going "fullscreen"); leave other moves alone so
        // tools like Special K don't end up fighting us in a loop
        WINDOWPOS* p = (WINDOWPOS*)lp;
        if (true) {
            static int logs;
            if (logs < 5) {
                logs++;
                char b[96];
                wsprintfA(b, "th06nc-tate: blocked landscape window resize %dx%d", p->cx, p->cy);
                logLine(b);
            }
            p->x = g_monRect.left; p->y = g_monRect.top;
            p->cx = g_monRect.right - g_monRect.left; p->cy = g_monRect.bottom - g_monRect.top;
            p->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        }
    }
    if (msg == WM_SIZE && g_portraitOut && g_fakeWv && wp != SIZE_MINIMIZED) lp = MAKELPARAM(g_fakeWv, g_fakeHv);
    if (overlayInput(h, msg, wp, lp)) return 0;
    return g_wndUnicode ? CallWindowProcW(g_origWndProc, h, msg, wp, lp)
                        : CallWindowProcA(g_origWndProc, h, msg, wp, lp);
}

static void subclassWindow(HWND hwnd) {
    if (hwnd && hwnd != g_gameHwnd) {
        g_gameHwnd = hwnd;
        g_wndUnicode = IsWindowUnicode(hwnd) != 0;
        g_origWndProc = g_wndUnicode ? (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc)
                                     : (WNDPROC)SetWindowLongPtrA(hwnd, GWLP_WNDPROC, (LONG_PTR)hkWndProc);
    }
}

static void installWindowLies(HWND hwnd) {
    static bool patched;
    if (!patched) {
        patched = true;
        BYTE* exe = (BYTE*)GetModuleHandleA(nullptr);
        IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(exe + ((IMAGE_DOS_HEADER*)exe)->e_lfanew);
        g_exeLo = exe;
        g_exeHi = exe + nt->OptionalHeader.SizeOfImage;
        BYTE* target = (BYTE*)(void*)GetProcAddress(GetModuleHandleA("user32.dll"), "GetClientRect");
        if (target) {
            BYTE patch[12] = { 0x48, 0xB8, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xE0 }; // mov rax, imm64; jmp rax
            ULONG_PTR a = (ULONG_PTR)(void*)hkGetClientRect;
            for (int i = 0; i < 8; i++) patch[2 + i] = (BYTE)(a >> (8 * i));
            DWORD old;
            if (VirtualProtect(target, sizeof(patch), PAGE_EXECUTE_READWRITE, &old)) {
                memcpy(target, patch, sizeof(patch));
                VirtualProtect(target, sizeof(patch), old, &old);
                FlushInstructionCache(GetCurrentProcess(), target, sizeof(patch));
                logLine("th06nc-tate: GetClientRect hooked");
            }
        }
    }
    subclassWindow(hwnd);
}

static void releaseFake() {
    SafeRelease(g_fakeSrv);
    SafeRelease(g_fake);
    g_fakeW = g_fakeH = 0;
    g_fakeWv = g_fakeHv = 0;
}

static UINT g_reqW, g_reqH;   // size the game asked for (its resolution setting)
static bool monitorPortrait(HWND h);

static bool is169(UINT w, UINT h) {
    if (!w || !h || h > w) return false;
    float a = (float)w / h;
    return a > 1.74f && a < 1.82f;
}

// Non-16:9 output (portrait or 4:3 desktop): the game gets a fake landscape backbuffer at its own resolution,
// and we lay it out onto the real output ourselves.
static void updateFake(IDXGISwapChain* sc) {
    releaseFake();
    g_portraitOut = false;
    g_portraitOutFlag = false;
    DXGI_SWAP_CHAIN_DESC d;
    if (!o_GetDesc || FAILED(o_GetDesc(sc, &d))) return;
    UINT w = d.BufferDesc.Width, h = d.BufferDesc.Height;
    if (!w || !h) return;
    if (!g_fakeFs && (is169(w, h) || !monitorPortrait(d.OutputWindow))) return;
    if (!ensureRenderer(sc)) return;

    UINT fw, fh;
    if (g_reqW > g_reqH && g_reqH > 0) { fw = g_reqW; fh = g_reqH; }   // the game's own resolution
    else { fw = w > h ? w : h; fh = w > h ? h : w; }                  // landscape version of the output

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = fw; td.Height = fh;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = d.BufferDesc.Format;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(g_dev->CreateTexture2D(&td, nullptr, &g_fake))) { logLine("th06nc-tate: fake backbuffer failed"); return; }
    D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
    sv.Format = typedFormat(td.Format);
    sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;
    if (FAILED(g_dev->CreateShaderResourceView(g_fake, &sv, &g_fakeSrv))) { releaseFake(); return; }
    g_fakeW = fw; g_fakeH = fh;
    g_fakeWv = fw; g_fakeHv = fh;
    g_outW = w; g_outH = h;
    g_portraitOut = true;
    g_portraitOutFlag = true;
    installWindowLies(d.OutputWindow);
    static UINT lw, lh, lo, lp;
    if (lw != fw || lh != fh || lo != w || lp != h) {
        lw = fw; lh = fh; lo = w; lp = h;
        char b[128];
        wsprintfA(b, "th06nc-tate: non-16:9 output, game renders %ux%u, output %ux%u", fw, fh, w, h);
        logLine(b);
    }
}

static HRESULT STDMETHODCALLTYPE hkGetBuffer(IDXGISwapChain* sc, UINT idx, REFIID riid, void** pp) {
    if (overlayCall(g_slBuf, __builtin_return_address(0), sc)) return o_GetBuffer(sc, idx, riid, pp);
    if (g_portraitOut && g_fake) return g_fake->QueryInterface(riid, pp);
    return o_GetBuffer(sc, idx, riid, pp);
}

static HRESULT STDMETHODCALLTYPE hkGetDesc(IDXGISwapChain* sc, DXGI_SWAP_CHAIN_DESC* d) {
    if (overlayCall(g_slDesc, __builtin_return_address(0), sc)) return o_GetDesc(sc, d);
    HRESULT hr = o_GetDesc(sc, d);
    if (SUCCEEDED(hr) && d && g_portraitOut && g_fake) { d->BufferDesc.Width = g_fakeW; d->BufferDesc.Height = g_fakeH; }
    if (SUCCEEDED(hr) && d && g_fakeFs) d->Windowed = !g_fsState;
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkGetDesc1(IDXGISwapChain1* sc, DXGI_SWAP_CHAIN_DESC1* d) {
    if (overlayCall(g_slDesc1, __builtin_return_address(0), sc)) return o_GetDesc1(sc, d);
    HRESULT hr = o_GetDesc1(sc, d);
    if (SUCCEEDED(hr) && d && g_portraitOut && g_fake) { d->Width = g_fakeW; d->Height = g_fakeH; }
    return hr;
}

static bool monitorPortrait(HWND h);
static HRESULT STDMETHODCALLTYPE hkResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    releaseCopy();
    releaseFake();
    g_portraitOut = false;
    g_portraitOutFlag = false;
    // the game's size goes to the fake buffer; the real buffer always follows the output
    if (w && h && w > h) { g_reqW = w; g_reqH = h; }
    DXGI_SWAP_CHAIN_DESC d;
    if (o_GetDesc && SUCCEEDED(o_GetDesc(sc, &d)) && (g_fakeFs || monitorPortrait(d.OutputWindow))) {
        if (g_fakeFs) { w = g_monRect.right - g_monRect.left; h = g_monRect.bottom - g_monRect.top; }
        else { RECT rc = realClient(d.OutputWindow); if (rc.right > 0 && rc.bottom > 0) { w = rc.right; h = rc.bottom; } }
    }
    static UINT lastW, lastH, repeats;
    if (w != lastW || h != lastH || ++repeats < 3) {
        if (w != lastW || h != lastH) repeats = 0;
        char b[96];
        wsprintfA(b, "th06nc-tate: ResizeBuffers %ux%u", w, h);
        logLine(b);
    }
    lastW = w; lastH = h;
    HRESULT hr = o_ResizeBuffers(sc, n, w, h, f, fl);
    if (SUCCEEDED(hr)) updateFake(sc);
    return hr;
}

static void hookSwapChain(IUnknown* obj) {
    if (!obj) return;
    IDXGISwapChain* sc = nullptr;
    if (FAILED(obj->QueryInterface(__uuidof(IDXGISwapChain), (void**)&sc))) return;
    void* p;
    if ((p = patchVT(sc, 8, (void*)hkPresent)))        o_Present       = (PFN_Present)p;
    if ((p = patchVT(sc, 9, (void*)hkGetBuffer)))      o_GetBuffer     = (PFN_GetBuffer)p;
    if ((p = patchVT(sc, 12, (void*)hkGetDesc)))       o_GetDesc       = (PFN_GetDesc)p;
    if ((p = patchVT(sc, 13, (void*)hkResizeBuffers))) o_ResizeBuffers = (PFN_ResizeBuffers)p;
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1))) {
        if ((p = patchVT(sc1, 18, (void*)hkGetDesc1))) o_GetDesc1 = (PFN_GetDesc1)p;
        if ((p = patchVT(sc1, 22, (void*)hkPresent1))) o_Present1 = (PFN_Present1)p;
        sc1->Release();
    }
    DXGI_SWAP_CHAIN_DESC d = {};
    if (SUCCEEDED(o_GetDesc(sc, &d))) {
        RECT rc = realClient(d.OutputWindow);
        char b[160];
        wsprintfA(b, "th06nc-tate: swapchain %ux%u windowed=%d window client %ldx%ld",
                  d.BufferDesc.Width, d.BufferDesc.Height, d.Windowed, rc.right, rc.bottom);
        logLine(b);
    }
    subclassWindow(d.OutputWindow); // for the Alt+0 window's mouse and keyboard
    updateFake(sc);
    sc->Release();
}

// ---------------------------------------------------------------- factory hooks
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChain)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_CreateSwapChainForHwnd)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
                                                               const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
static PFN_CreateSwapChain        o_CreateSwapChain;
static PFN_CreateSwapChainForHwnd o_CreateSwapChainForHwnd;

static bool monitorPortrait(HWND h) {
    static bool seenPortrait; // a fullscreen mode switch can make the monitor look landscape later
    if (g_portraitDesktop == 0) return false;
    if (seenPortrait) return true;
    MONITORINFO mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) return g_portraitDesktop == 1;
    if (g_portraitDesktop == 1) { g_monRect = mi.rcMonitor; seenPortrait = true; return true; }
    seenPortrait = !is169(mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
    if (seenPortrait) {
        g_monRect = mi.rcMonitor;
        char b[96];
        wsprintfA(b, "th06nc-tate: desktop %ldx%ld is not 16:9", mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top);
        logLine(b);
    }
    return seenPortrait;
}

typedef HRESULT (STDMETHODCALLTYPE* PFN_SetFs)(IDXGISwapChain*, BOOL, IDXGIOutput*);
typedef HRESULT (STDMETHODCALLTYPE* PFN_GetFs)(IDXGISwapChain*, BOOL*, IDXGIOutput**);
typedef HRESULT (STDMETHODCALLTYPE* PFN_ResizeTarget)(IDXGISwapChain*, const DXGI_MODE_DESC*);
static PFN_SetFs o_SetFs;
static PFN_GetFs o_GetFs;
static PFN_ResizeTarget o_ResizeTarget;

static HRESULT STDMETHODCALLTYPE hkSetFs(IDXGISwapChain* sc, BOOL fs, IDXGIOutput* o) {
    if (g_fakeFs) { g_fsState = fs; return S_OK; }
    return o_SetFs(sc, fs, o);
}
static HRESULT STDMETHODCALLTYPE hkGetFs(IDXGISwapChain* sc, BOOL* fs, IDXGIOutput** o) {
    if (g_fakeFs) { if (fs) *fs = g_fsState; if (o) *o = nullptr; return S_OK; }
    return o_GetFs(sc, fs, o);
}
static HRESULT STDMETHODCALLTYPE hkResizeTarget(IDXGISwapChain* sc, const DXGI_MODE_DESC* m) {
    if (g_fakeFs) return S_OK;
    return o_ResizeTarget(sc, m);
}

// Portrait monitor + exclusive fullscreen: run as a borderless window covering the monitor instead,
// so no display mode change squashes the picture.
static void coverMonitor(IDXGISwapChain* sc, HWND hwnd) {
    void* p;
    if ((p = patchVT(sc, 10, (void*)hkSetFs)))        o_SetFs = (PFN_SetFs)p;
    if ((p = patchVT(sc, 11, (void*)hkGetFs)))        o_GetFs = (PFN_GetFs)p;
    if ((p = patchVT(sc, 14, (void*)hkResizeTarget))) o_ResizeTarget = (PFN_ResizeTarget)p;
    g_fsState = TRUE;
    installWindowLies(hwnd);
    LONG_PTR st = GetWindowLongPtrA(hwnd, GWL_STYLE);
    st &= ~(LONG_PTR)(WS_CAPTION | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX);
    st |= WS_POPUP;
    SetWindowLongPtrA(hwnd, GWL_STYLE, st);
    SetWindowPos(hwnd, HWND_TOP, g_monRect.left, g_monRect.top, g_monRect.right - g_monRect.left,
                 g_monRect.bottom - g_monRect.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    logLine("th06nc-tate: fullscreen on non-16:9 desktop -> screen-sized window");
}

static HRESULT STDMETHODCALLTYPE hkCreateSwapChain(IDXGIFactory* f, IUnknown* dev, DXGI_SWAP_CHAIN_DESC* d, IDXGISwapChain** sc) {
    DXGI_SWAP_CHAIN_DESC local;
    if (d && d->BufferDesc.Width > d->BufferDesc.Height && monitorPortrait(d->OutputWindow)) {
        // non-16:9 desktop: the real buffer matches the screen, the game keeps drawing 16:9 into a fake one
        local = *d;
        g_reqW = local.BufferDesc.Width; g_reqH = local.BufferDesc.Height;
        bool wantFs = !local.Windowed;
        local.Windowed = TRUE;
        if (wantFs) g_fakeFs = true;
        if (g_fakeFs) {
            local.BufferDesc.Width = g_monRect.right - g_monRect.left;
            local.BufferDesc.Height = g_monRect.bottom - g_monRect.top;
        } else {
            RECT rc = realClient(local.OutputWindow);
            if (rc.right > 0 && rc.bottom > 0) { local.BufferDesc.Width = rc.right; local.BufferDesc.Height = rc.bottom; }
        }
        d = &local;
        logLine("th06nc-tate: non-16:9 desktop, real buffer follows the screen");
    }
    HRESULT hr = o_CreateSwapChain(f, dev, d, sc);
    if (SUCCEEDED(hr) && sc && g_fakeFs) coverMonitor(*sc, d->OutputWindow);
    if (SUCCEEDED(hr) && sc) hookSwapChain(*sc);
    return hr;
}

static HRESULT STDMETHODCALLTYPE hkCreateSwapChainForHwnd(IDXGIFactory2* f, IUnknown* dev, HWND w, const DXGI_SWAP_CHAIN_DESC1* d,
                                                          const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fd, IDXGIOutput* o, IDXGISwapChain1** sc) {
    DXGI_SWAP_CHAIN_DESC1 local;
    DXGI_SWAP_CHAIN_FULLSCREEN_DESC localFd;
    if (d && d->Width > d->Height && d->Height > 0 && monitorPortrait(w)) {
        local = *d;
        g_reqW = local.Width; g_reqH = local.Height;
        if (fd && !fd->Windowed) { localFd = *fd; localFd.Windowed = TRUE; fd = &localFd; g_fakeFs = true; }
        if (g_fakeFs) {
            local.Width = g_monRect.right - g_monRect.left;
            local.Height = g_monRect.bottom - g_monRect.top;
        } else {
            RECT rc = realClient(w);
            if (rc.right > 0 && rc.bottom > 0) { local.Width = rc.right; local.Height = rc.bottom; }
        }
        d = &local;
        logLine("th06nc-tate: non-16:9 desktop, real buffer follows the screen");
    }
    HRESULT hr = o_CreateSwapChainForHwnd(f, dev, w, d, fd, o, sc);
    if (SUCCEEDED(hr) && sc && g_fakeFs) coverMonitor(*sc, w);
    if (SUCCEEDED(hr) && sc) hookSwapChain(*sc);
    return hr;
}

static void hookFactory(void* obj) {
    if (!obj) return;
    IUnknown* u = (IUnknown*)obj;
    IDXGIFactory* f = nullptr;
    void* p;
    if (SUCCEEDED(u->QueryInterface(__uuidof(IDXGIFactory), (void**)&f))) {
        if ((p = patchVT(f, 10, (void*)hkCreateSwapChain))) o_CreateSwapChain = (PFN_CreateSwapChain)p;
        f->Release();
    }
    IDXGIFactory2* f2 = nullptr;
    if (SUCCEEDED(u->QueryInterface(__uuidof(IDXGIFactory2), (void**)&f2))) {
        if ((p = patchVT(f2, 15, (void*)hkCreateSwapChainForHwnd))) o_CreateSwapChainForHwnd = (PFN_CreateSwapChainForHwnd)p;
        f2->Release();
    }
}

// ---------------------------------------------------------------- dxgi.dll proxy exports
static HMODULE g_real;
static FARPROC realProc(const char* name) {
    if (!g_real) {
        char p[MAX_PATH];
        GetSystemDirectoryA(p, MAX_PATH);
        lstrcatA(p, "\\dxgi.dll");
        g_real = LoadLibraryA(p);
        if (!g_real) { logLine("th06nc-tate: could not load system dxgi.dll"); return nullptr; }
    }
    return GetProcAddress(g_real, name);
}

typedef HRESULT (WINAPI* PFN_CF)(REFIID, void**);
typedef HRESULT (WINAPI* PFN_CF2)(UINT, REFIID, void**);
typedef HRESULT (WINAPI* PFN_Void)(void);
typedef HRESULT (WINAPI* PFN_Dbg1)(UINT, REFIID, void**);
typedef void    (WINAPI* PFN_Report)(UINT);

extern "C" HRESULT WINAPI CreateDXGIFactory(REFIID riid, void** f) {
    PFN_CF fn = (PFN_CF)(void*)realProc("CreateDXGIFactory");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(riid, f);
    if (SUCCEEDED(hr) && f) hookFactory(*f);
    return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory1(REFIID riid, void** f) {
    PFN_CF fn = (PFN_CF)(void*)realProc("CreateDXGIFactory1");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(riid, f);
    if (SUCCEEDED(hr) && f) hookFactory(*f);
    return hr;
}

extern "C" HRESULT WINAPI CreateDXGIFactory2(UINT flags, REFIID riid, void** f) {
    PFN_CF2 fn = (PFN_CF2)(void*)realProc("CreateDXGIFactory2");
    if (!fn) return E_FAIL;
    HRESULT hr = fn(flags, riid, f);
    if (SUCCEEDED(hr) && f) hookFactory(*f);
    return hr;
}

extern "C" HRESULT WINAPI DXGIGetDebugInterface1(UINT flags, REFIID riid, void** p) {
    PFN_Dbg1 fn = (PFN_Dbg1)(void*)realProc("DXGIGetDebugInterface1");
    return fn ? fn(flags, riid, p) : E_NOINTERFACE;
}

extern "C" HRESULT WINAPI DXGIDeclareAdapterRemovalSupport(void) {
    PFN_Void fn = (PFN_Void)(void*)realProc("DXGIDeclareAdapterRemovalSupport");
    return fn ? fn() : S_OK;
}

extern "C" HRESULT WINAPI DXGIDisableVBlankVirtualization(void) {
    PFN_Void fn = (PFN_Void)(void*)realProc("DXGIDisableVBlankVirtualization");
    return fn ? fn() : S_OK;
}

extern "C" void WINAPI DXGIReportAdapterConfiguration(UINT v) {
    PFN_Report fn = (PFN_Report)(void*)realProc("DXGIReportAdapterConfiguration");
    if (fn) fn(v);
}

// ---------------------------------------------------------------- crash logging
static volatile LONG g_crashLogs;
static LONG WINAPI crashLogger(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code < 0xC0000000 || !g_dev) return EXCEPTION_CONTINUE_SEARCH; // real faults, after startup
    if (InterlockedIncrement(&g_crashLogs) > 4) return EXCEPTION_CONTINUE_SEARCH;
    BYTE* at = (BYTE*)ep->ExceptionRecord->ExceptionAddress;
    char mod[MAX_PATH] = "(no module)";
    HMODULE hm = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)at, &hm) && hm)
        GetModuleFileNameA(hm, mod, MAX_PATH);
    char b[MAX_PATH + 120];
    wsprintfA(b, "th06nc-tate: FAULT %08lx in %s +%lx", code, mod, (unsigned long)(at - (BYTE*)hm));
    logLine(b);
    ULONG_PTR* sp = (ULONG_PTR*)ep->ContextRecord->Rsp;
    int found = 0;
    for (int i = 0; i < 1024 && found < 10; i++) {
        BYTE* v = (BYTE*)sp[i];
        if (v >= g_selfBase && v < g_selfBase + 0x100000) {
            wsprintfA(b, "th06nc-tate:   stack dll+%lx", (unsigned long)(v - g_selfBase));
            logLine(b);
            found++;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// ---------------------------------------------------------------- entry
extern "C" BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        g_selfBase = (BYTE*)inst;
        AddVectoredExceptionHandler(1, crashLogger);
        InitializeCriticalSection(&g_lock);
        GetModuleFileNameA(inst, g_dir, MAX_PATH);
        char* slash = g_dir;
        for (char* q = g_dir; *q; q++) if (*q == '\\' || *q == '/') slash = q + 1;
        *slash = 0;
        lstrcpyA(g_iniPath, g_dir);
        lstrcatA(g_iniPath, "th06nc-tate.ini");
        lstrcpyA(g_logPath, g_dir);
        lstrcatA(g_logPath, "th06nc-tate-log.txt");
        loadConfig();
    }
    return TRUE;
}
