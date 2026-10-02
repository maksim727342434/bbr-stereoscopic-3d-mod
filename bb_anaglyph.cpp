// Beach Buggy Racing Remix - red/cyan anaglyph 3D filter
// Proxy for SDL2.dll (original renamed to SDL2_orig.dll).
// Hooks IDXGISwapChain::Present / Present1 via vtable patching, grabs the scene depth buffer
// (by tracking OMSetRenderTargets + Draw calls) and applies a depth-based stereo reprojection.
//
// Hotkeys (in game):
//   F5  anaglyph type (Dubois / color / gray)
//   F6  effect on/off
//   F7  mode: 0 = depth based, 1 = fake depth (screen Y), 2 = depth debug view
//   F8 / F9    separation  - / +
//   F10 / F11  convergence - / +
//   F12 swap eyes
//   Insert reversed-Z toggle
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdarg.h>


// ---------------------------------------------------------------- tiny CRT-free helpers
// (the DLL must not depend on msvcrt.dll: the game is a UWP/AppContainer process)
extern "C" {
void* memset(void* d, int v, size_t n) { unsigned char* p = (unsigned char*)d; while (n--) *p++ = (unsigned char)v; return d; }
void* memcpy(void* d, const void* s, size_t n) { unsigned char* p = (unsigned char*)d; const unsigned char* q = (const unsigned char*)s; while (n--) *p++ = *q++; return d; }
}
static size_t Strlen(const char* s) { size_t n = 0; while (s[n]) n++; return n; }
static bool Streq(const char* a, const char* b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static int Atoi(const char* s) { int sign = 1, v = 0; if (*s == '-') { sign = -1; s++; } while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v * sign; }
static float Atof(const char* s)
{
    float sign = 1.f, v = 0.f; if (*s == '-') { sign = -1.f; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10.f + (float)(*s++ - '0');
    if (*s == '.') { s++; float f = 0.1f; while (*s >= '0' && *s <= '9') { v += f * (float)(*s++ - '0'); f *= 0.1f; } }
    return v * sign;
}
static int FmtV(char* out, int cap, const char* fmt, va_list ap)
{
    int n = 0;
    #define PUT(c) do { if (n < cap - 1) out[n] = (c); n++; } while (0)
    for (; *fmt; fmt++)
    {
        if (*fmt != '%') { PUT(*fmt); continue; }
        fmt++;
        bool zero = false; int width = 0; bool lng = false;
        if (*fmt == '0') { zero = true; fmt++; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') { lng = true; fmt++; }
        char tmp[24]; int tl = 0; const char* str = nullptr;
        char c = *fmt;
        if (c == 's') { str = va_arg(ap, const char*); if (!str) str = "(null)"; tl = (int)Strlen(str); }
        else if (c == 'c') { tmp[0] = (char)va_arg(ap, int); tl = 1; str = tmp; }
        else if (c == 'd' || c == 'u' || c == 'x' || c == 'p')
        {
            unsigned long long v; bool neg = false;
            if (c == 'p') { v = (unsigned long long)va_arg(ap, void*); }
            else if (c == 'd') { long long sv = lng ? va_arg(ap, long long) : (long long)va_arg(ap, int); if (sv < 0) { neg = true; sv = -sv; } v = (unsigned long long)sv; }
            else { v = lng ? va_arg(ap, unsigned long long) : (unsigned long long)va_arg(ap, unsigned int); }
            unsigned base = (c == 'x' || c == 'p') ? 16 : 10;
            char rev[24]; int rl = 0;
            if (v == 0) rev[rl++] = '0';
            while (v) { unsigned dgt = (unsigned)(v % base); rev[rl++] = (char)(dgt < 10 ? '0' + dgt : 'a' + dgt - 10); v /= base; }
            if (neg) tmp[tl++] = '-';
            while (rl) tmp[tl++] = rev[--rl];
            str = tmp;
        }
        else { PUT('%'); if (c) PUT(c); if (!c) break; continue; }
        for (int i = tl; i < width; i++) PUT(zero ? '0' : ' ');
        for (int i = 0; i < tl; i++) PUT(str[i]);
    }
    if (cap > 0) out[n < cap ? n : cap - 1] = 0;
    #undef PUT
    return n < cap ? n : cap - 1;
}
static int Fmt(char* out, int cap, const char* fmt, ...) { va_list ap; va_start(ap, fmt); int n = FmtV(out, cap, fmt, ap); va_end(ap); return n; }
// float -> "int.frac" pieces (non-negative values only)
#define FI(v) ((int)(v))
#define FF(v) ((int)(((v) - (float)(int)(v)) * 1000.f + 0.5f))

// ---------------------------------------------------------------- logging / config
static char   g_dir[1024];
static HANDLE g_log = INVALID_HANDLE_VALUE;

static void Log(const char* fmt, ...)
{
    if (g_log == INVALID_HANDLE_VALUE) return;
    char buf[640];
    va_list ap; va_start(ap, fmt);
    int n = FmtV(buf, sizeof(buf) - 3, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if (n > (int)sizeof(buf) - 3) n = sizeof(buf) - 3;
    buf[n++] = '\r'; buf[n++] = '\n';
    DWORD w; WriteFile(g_log, buf, (DWORD)n, &w, nullptr);
    FlushFileBuffers(g_log);
}

struct Cfg
{
    int   enabled   = 1;
    int   mode      = 0;      // 0 depth, 1 fake, 2 debug depth
    int   type      = 0;      // 0 dubois, 1 color, 2 gray
    int   swapEyes  = 0;
    int   reversedZ = 0;
    float sep       = 0.020f; // total disparity, fraction of screen width
    float conv      = 8.0f;   // distance (world units) that stays on the screen plane
    float nearZ     = 0.5f;
    float farZ      = 2000.0f;
};
static Cfg g_cfg;

static void OpenPath(const char* name, char* out, size_t cap)
{
    Fmt(out, (int)cap, "%s%s", g_dir, name);
}

static void SaveCfg()
{
    if (!g_dir[0]) return;
    char path[1200]; OpenPath("anaglyph.ini", path, sizeof(path));
    wchar_t wp[1200]; MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1200);
    HANDLE h = CreateFileW(wp, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    char t[1024];
    int n = Fmt(t, sizeof(t),
        "enabled=%d\r\nmode=%d\r\ntype=%d\r\nswap_eyes=%d\r\nreversed_z=%d\r\nseparation=%d.%03d\r\nconvergence=%d.%03d\r\nnear=%d.%03d\r\nfar=%d.%03d\r\n",
        g_cfg.enabled, g_cfg.mode, g_cfg.type, g_cfg.swapEyes, g_cfg.reversedZ,
        FI(g_cfg.sep), FF(g_cfg.sep), FI(g_cfg.conv), FF(g_cfg.conv), FI(g_cfg.nearZ), FF(g_cfg.nearZ), FI(g_cfg.farZ), FF(g_cfg.farZ));
    DWORD w; WriteFile(h, t, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}

static void LoadCfg()
{
    char path[1200]; OpenPath("anaglyph.ini", path, sizeof(path));
    wchar_t wp[1200]; MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1200);
    HANDLE h = CreateFileW(wp, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { SaveCfg(); return; }
    char buf[2048]; DWORD r = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &r, nullptr);
    CloseHandle(h);
    buf[r] = 0;
    char* p = buf;
    while (*p)
    {
        char* line = p;
        while (*p && *p != '\r' && *p != '\n') p++;
        if (*p) { *p++ = 0; }
        char* eq = line; while (*eq && *eq != '=') eq++;
        if (*eq != '=') continue;
        *eq = 0; const char* k = line; const char* v = eq + 1;
        if (Streq(k, "enabled")) g_cfg.enabled = Atoi(v);
        else if (Streq(k, "mode")) g_cfg.mode = Atoi(v);
        else if (Streq(k, "type")) g_cfg.type = Atoi(v);
        else if (Streq(k, "swap_eyes")) g_cfg.swapEyes = Atoi(v);
        else if (Streq(k, "reversed_z")) g_cfg.reversedZ = Atoi(v);
        else if (Streq(k, "separation")) g_cfg.sep = Atof(v);
        else if (Streq(k, "convergence")) g_cfg.conv = Atof(v);
        else if (Streq(k, "near")) g_cfg.nearZ = Atof(v);
        else if (Streq(k, "far")) g_cfg.farZ = Atof(v);
    }
}

// ---------------------------------------------------------------- HLSL
static const char* kHLSL = R"HLSL(
cbuffer CB : register(b0) { float4 P0; float4 P1; float4 P2; };
Texture2D sceneTex : register(t0);
Texture2D depthTex : register(t1);
SamplerState linS : register(s0);
SamplerState ptS  : register(s1);
struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((id << 1) & 2, id & 2);
    o.pos = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

float FakeZ(float y, float conv)
{
    float d = y - 0.40;
    if (d < 0.02) return conv * 12.0;
    return conv * 0.25 / d;
}

float LinZ(float d, float nz, float fz, float rev)
{
    if (rev > 0.5) d = 1.0 - d;
    return nz * fz / max(fz - d * (fz - nz), 1e-4);
}

float4 PSMain(VSOut i) : SV_Target
{
    float sep = P0.x, conv = P0.y, nz = P0.z, fz = P0.w;
    float mode = P1.x, rev = P1.y, has = P1.z, typ = P1.w, swp = P2.x;
    float z;
    if (mode < 0.5 && has > 0.5) z = LinZ(depthTex.SampleLevel(ptS, i.uv, 0).r, nz, fz, rev);
    else                         z = FakeZ(i.uv.y, conv);

    if (mode > 1.5)
    {
        if (has < 0.5) return float4(1, 0, 0, 1);
        float v = saturate(1.0 - z / (conv * 4.0));
        return float4(v, v, v, 1);
    }

    float n = clamp(conv / max(z, 1e-3) - 1.0, -1.0, 2.0);
    float p = sep * n * 0.5;
    float s = (swp > 0.5) ? -1.0 : 1.0;
    float3 L = sceneTex.SampleLevel(linS, float2(i.uv.x - p * s, i.uv.y), 0).rgb;
    float3 R = sceneTex.SampleLevel(linS, float2(i.uv.x + p * s, i.uv.y), 0).rgb;

    float3 o;
    if (typ < 0.5)
    {
        o.r = dot(L, float3( 0.437,  0.449,  0.164)) + dot(R, float3(-0.011, -0.032, -0.007));
        o.g = dot(L, float3(-0.062, -0.062, -0.024)) + dot(R, float3( 0.377,  0.761,  0.009));
        o.b = dot(L, float3(-0.048, -0.050, -0.017)) + dot(R, float3(-0.026, -0.093,  1.234));
    }
    else if (typ < 1.5)
    {
        o = float3(L.r, R.g, R.b);
    }
    else
    {
        float ll = dot(L, float3(0.299, 0.587, 0.114));
        float rl = dot(R, float3(0.299, 0.587, 0.114));
        o = float3(ll, rl, rl);
    }
    return float4(saturate(o), 1);
}
)HLSL";

typedef HRESULT (WINAPI *PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

// ---------------------------------------------------------------- D3D state
typedef HRESULT (STDMETHODCALLTYPE *PresentFn)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT (STDMETHODCALLTYPE *Present1Fn)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef void (STDMETHODCALLTYPE *OMSetRTFn)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
typedef void (STDMETHODCALLTYPE *DrawIdxFn)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE *DrawFn)(ID3D11DeviceContext*, UINT, UINT);
typedef void (STDMETHODCALLTYPE *DrawIdxInstFn)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE *DrawInstFn)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);

static PresentFn     g_oPresent;
static Present1Fn    g_oPresent1;
static OMSetRTFn     g_oOMSetRT;
static DrawIdxFn     g_oDrawIdx;
static DrawFn        g_oDraw;
static DrawIdxInstFn g_oDrawIdxInst;
static DrawInstFn    g_oDrawInst;

static bool g_inside = false;

struct Cand
{
    ID3D11DepthStencilView* dsv;
    ID3D11Texture2D*        tex;   // AddRef'd until end of frame
    UINT w, h, samples;
    DXGI_FORMAT fmt;
    unsigned draws;
};
static Cand  g_c[24];
static int   g_nc = 0;
static Cand* g_cur = nullptr;

static void ResetCands()
{
    for (int i = 0; i < g_nc; i++) if (g_c[i].tex) g_c[i].tex->Release();
    g_nc = 0; g_cur = nullptr;
}

static Cand* FindOrAdd(ID3D11DepthStencilView* dsv)
{
    for (int i = 0; i < g_nc; i++) if (g_c[i].dsv == dsv) return &g_c[i];
    if (g_nc >= 24) return nullptr;
    ID3D11Resource* res = nullptr;
    dsv->GetResource(&res);
    if (!res) return nullptr;
    ID3D11Texture2D* tex = nullptr;
    res->QueryInterface(IID_ID3D11Texture2D, (void**)&tex);
    res->Release();
    if (!tex) return nullptr;
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    Cand* c = &g_c[g_nc++];
    c->dsv = dsv; c->tex = tex; c->w = d.Width; c->h = d.Height; c->samples = d.SampleDesc.Count;
    c->fmt = d.Format; c->draws = 0;
    return c;
}

static void STDMETHODCALLTYPE hkOMSetRT(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv)
{
    if (!g_inside) g_cur = dsv ? FindOrAdd(dsv) : nullptr;
    g_oOMSetRT(c, n, rtv, dsv);
}
static void STDMETHODCALLTYPE hkDrawIdx(ID3D11DeviceContext* c, UINT a, UINT b, INT d)
{ if (!g_inside && g_cur) g_cur->draws++; g_oDrawIdx(c, a, b, d); }
static void STDMETHODCALLTYPE hkDraw(ID3D11DeviceContext* c, UINT a, UINT b)
{ if (!g_inside && g_cur) g_cur->draws++; g_oDraw(c, a, b); }
static void STDMETHODCALLTYPE hkDrawIdxInst(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f)
{ if (!g_inside && g_cur) g_cur->draws++; g_oDrawIdxInst(c, a, b, d, e, f); }
static void STDMETHODCALLTYPE hkDrawInst(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e)
{ if (!g_inside && g_cur) g_cur->draws++; g_oDrawInst(c, a, b, d, e); }

// our resources (created on the game's device)
struct Res
{
    ID3D11Device*            dev = nullptr;
    ID3D11VertexShader*      vs = nullptr;
    ID3D11PixelShader*       ps = nullptr;
    ID3D11Buffer*            cb = nullptr;
    ID3D11SamplerState*      sLin = nullptr;
    ID3D11SamplerState*      sPt = nullptr;
    ID3D11RasterizerState*   rs = nullptr;
    ID3D11BlendState*        bs = nullptr;
    ID3D11DepthStencilState* dss = nullptr;
    ID3D11Texture2D*         sceneCopy = nullptr;
    ID3D11ShaderResourceView* sceneSRV = nullptr;
    UINT sceneW = 0, sceneH = 0; DXGI_FORMAT sceneFmt = DXGI_FORMAT_UNKNOWN;
    ID3D11Texture2D*         depthCopy = nullptr;
    ID3D11ShaderResourceView* depthSRV = nullptr;
    UINT dW = 0, dH = 0; DXGI_FORMAT dFmt = DXGI_FORMAT_UNKNOWN;
    bool ready = false, failed = false;
};
static Res g;
static unsigned g_frame = 0;

static bool InitResources(ID3D11Device* dev)
{
    HMODULE mod = nullptr;
    typedef HMODULE (WINAPI *LPL)(LPCWSTR, DWORD);
    HMODULE kb = GetModuleHandleW(L"kernelbase.dll");
    LPL lpl = kb ? (LPL)GetProcAddress(kb, "LoadPackagedLibrary") : nullptr;
    if (lpl) mod = lpl(L"d3dcompiler_47.dll", 0);
    if (!mod) mod = LoadLibraryW(L"d3dcompiler_47.dll");
    if (!mod) { Log("ERROR: d3dcompiler_47.dll not found (copy it next to the exe)"); return false; }
    PFN_D3DCompile comp = (PFN_D3DCompile)GetProcAddress(mod, "D3DCompile");
    if (!comp) { Log("ERROR: D3DCompile export missing"); return false; }

    ID3DBlob *vsb = nullptr, *psb = nullptr, *err = nullptr;
    HRESULT hr = comp(kHLSL, Strlen(kHLSL), "bbana", nullptr, nullptr, "VSMain", "vs_4_0", 0, 0, &vsb, &err);
    if (FAILED(hr)) { Log("ERROR VS compile: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    hr = comp(kHLSL, Strlen(kHLSL), "bbana", nullptr, nullptr, "PSMain", "ps_4_0", 0, 0, &psb, &err);
    if (FAILED(hr)) { Log("ERROR PS compile: %s", err ? (const char*)err->GetBufferPointer() : "?"); return false; }
    dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &g.vs);
    dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &g.ps);
    vsb->Release(); psb->Release();
    if (!g.vs || !g.ps) { Log("ERROR creating shaders"); return false; }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 48; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    dev->CreateBuffer(&bd, nullptr, &g.cb);

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g.sLin);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    dev->CreateSamplerState(&sd, &g.sPt);

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    dev->CreateRasterizerState(&rd, &g.rs);

    D3D11_BLEND_DESC bld = {};
    bld.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    dev->CreateBlendState(&bld, &g.bs);

    D3D11_DEPTH_STENCIL_DESC dsd = {};
    dsd.DepthEnable = FALSE; dsd.StencilEnable = FALSE;
    dev->CreateDepthStencilState(&dsd, &g.dss);

    if (!g.cb || !g.sLin || !g.sPt || !g.rs || !g.bs || !g.dss) { Log("ERROR creating states"); return false; }
    g.dev = dev;
    Log("Resources ready");
    return true;
}

static bool DepthFormats(DXGI_FORMAT src, DXGI_FORMAT* tex, DXGI_FORMAT* srv)
{
    switch (src)
    {
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
        *tex = DXGI_FORMAT_R24G8_TYPELESS; *srv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; return true;
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS:
        *tex = DXGI_FORMAT_R32_TYPELESS; *srv = DXGI_FORMAT_R32_FLOAT; return true;
    case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS:
        *tex = DXGI_FORMAT_R16_TYPELESS; *srv = DXGI_FORMAT_R16_UNORM; return true;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS:
        *tex = DXGI_FORMAT_R32G8X24_TYPELESS; *srv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; return true;
    default: return false;
    }
}

struct Saved
{
    ID3D11VertexShader* vs; ID3D11PixelShader* ps; ID3D11GeometryShader* gs; ID3D11HullShader* hs; ID3D11DomainShader* ds;
    ID3D11ShaderResourceView* srv[2]; ID3D11SamplerState* smp[2]; ID3D11Buffer* cb;
    ID3D11RasterizerState* rs; D3D11_VIEWPORT vp[16]; UINT nvp;
    ID3D11BlendState* bs; FLOAT bf[4]; UINT sm; ID3D11DepthStencilState* dss; UINT sref;
    ID3D11RenderTargetView* rtv[8]; ID3D11DepthStencilView* dsv;
    ID3D11InputLayout* il; D3D11_PRIMITIVE_TOPOLOGY topo;
};

#define REL(p) do { if (p) { (p)->Release(); (p) = nullptr; } } while (0)

static void SaveState(ID3D11DeviceContext* c, Saved& s)
{
    memset(&s, 0, sizeof(s));
    c->VSGetShader(&s.vs, nullptr, nullptr);
    c->PSGetShader(&s.ps, nullptr, nullptr);
    c->GSGetShader(&s.gs, nullptr, nullptr);
    c->HSGetShader(&s.hs, nullptr, nullptr);
    c->DSGetShader(&s.ds, nullptr, nullptr);
    c->PSGetShaderResources(0, 2, s.srv);
    c->PSGetSamplers(0, 2, s.smp);
    c->PSGetConstantBuffers(0, 1, &s.cb);
    c->RSGetState(&s.rs);
    s.nvp = 16; c->RSGetViewports(&s.nvp, s.vp);
    c->OMGetBlendState(&s.bs, s.bf, &s.sm);
    c->OMGetDepthStencilState(&s.dss, &s.sref);
    c->OMGetRenderTargets(8, s.rtv, &s.dsv);
    c->IAGetInputLayout(&s.il);
    c->IAGetPrimitiveTopology(&s.topo);
}

static void RestoreState(ID3D11DeviceContext* c, Saved& s)
{
    c->VSSetShader(s.vs, nullptr, 0);
    c->PSSetShader(s.ps, nullptr, 0);
    c->GSSetShader(s.gs, nullptr, 0);
    c->HSSetShader(s.hs, nullptr, 0);
    c->DSSetShader(s.ds, nullptr, 0);
    c->PSSetShaderResources(0, 2, s.srv);
    c->PSSetSamplers(0, 2, s.smp);
    c->PSSetConstantBuffers(0, 1, &s.cb);
    c->RSSetState(s.rs);
    c->RSSetViewports(s.nvp, s.vp);
    c->OMSetBlendState(s.bs, s.bf, s.sm);
    c->OMSetDepthStencilState(s.dss, s.sref);
    c->OMSetRenderTargets(8, s.rtv, s.dsv);
    c->IASetInputLayout(s.il);
    c->IASetPrimitiveTopology(s.topo);
    REL(s.vs); REL(s.ps); REL(s.gs); REL(s.hs); REL(s.ds);
    REL(s.srv[0]); REL(s.srv[1]); REL(s.smp[0]); REL(s.smp[1]); REL(s.cb);
    REL(s.rs); REL(s.bs); REL(s.dss); REL(s.il); REL(s.dsv);
    for (int i = 0; i < 8; i++) REL(s.rtv[i]);
}

static void OnPresent(IDXGISwapChain* sc)
{
    if (g_inside || g.failed) { ResetCands(); return; }
    g_frame++;
    if (!g_cfg.enabled) { ResetCands(); return; }

    g_inside = true;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    ID3D11Texture2D* bb = nullptr;
    do
    {
        if (FAILED(sc->GetDevice(IID_ID3D11Device, (void**)&dev)) || !dev) break;
        if (!g.ready)
        {
            if (!InitResources(dev)) { g.failed = true; break; }
            g.ready = true;
        }
        if (dev != g.dev) break;
        dev->GetImmediateContext(&ctx);
        if (!ctx) break;
        if (FAILED(sc->GetBuffer(0, IID_ID3D11Texture2D, (void**)&bb)) || !bb) break;
        D3D11_TEXTURE2D_DESC bd; bb->GetDesc(&bd);
        if (bd.SampleDesc.Count > 1) break;

        // scene copy
        if (!g.sceneCopy || g.sceneW != bd.Width || g.sceneH != bd.Height || g.sceneFmt != bd.Format)
        {
            REL(g.sceneSRV); REL(g.sceneCopy);
            D3D11_TEXTURE2D_DESC cd = bd;
            cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.MiscFlags = 0; cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0;
            cd.MipLevels = 1; cd.ArraySize = 1;
            if (FAILED(dev->CreateTexture2D(&cd, nullptr, &g.sceneCopy))) { Log("ERROR scene copy tex fmt=%d", (int)bd.Format); g.failed = true; break; }
            if (FAILED(dev->CreateShaderResourceView(g.sceneCopy, nullptr, &g.sceneSRV))) { Log("ERROR scene SRV"); g.failed = true; break; }
            g.sceneW = bd.Width; g.sceneH = bd.Height; g.sceneFmt = bd.Format;
            Log("Backbuffer %ux%u fmt=%d", bd.Width, bd.Height, (int)bd.Format);
        }
        ctx->CopyResource(g.sceneCopy, bb);

        // pick depth candidate
        Cand* best = nullptr;
        for (int i = 0; i < g_nc; i++)
        {
            Cand& c = g_c[i];
            bool aspectOk = (c.w * bd.Height * 100 >= bd.Width * c.h * 96) && (c.w * bd.Height * 100 <= bd.Width * c.h * 104);
            if (!aspectOk || c.w * 4 < bd.Width || c.samples > 1) continue;
            DXGI_FORMAT a, b; if (!DepthFormats(c.fmt, &a, &b)) continue;
            if (!best || c.draws > best->draws) best = &c;
        }
        if (g_frame < 6 || (g_frame % 900) == 0)
        {
            Log("frame %u: %d depth candidates", g_frame, g_nc);
            for (int i = 0; i < g_nc; i++)
                Log("   cand %dx%d fmt=%d ms=%u draws=%u%s", g_c[i].w, g_c[i].h, (int)g_c[i].fmt, g_c[i].samples, g_c[i].draws, (&g_c[i] == best) ? "  <== chosen" : "");
        }
        bool hasDepth = false;
        if (best)
        {
            DXGI_FORMAT tf = DXGI_FORMAT_UNKNOWN, sf = DXGI_FORMAT_UNKNOWN; DepthFormats(best->fmt, &tf, &sf);
            D3D11_TEXTURE2D_DESC sd; best->tex->GetDesc(&sd);
            if (!g.depthCopy || g.dW != sd.Width || g.dH != sd.Height || g.dFmt != sd.Format)
            {
                REL(g.depthSRV); REL(g.depthCopy);
                D3D11_TEXTURE2D_DESC cd = sd;
                cd.Format = tf; cd.BindFlags = D3D11_BIND_SHADER_RESOURCE; cd.MiscFlags = 0; cd.Usage = D3D11_USAGE_DEFAULT; cd.CPUAccessFlags = 0;
                HRESULT hr = dev->CreateTexture2D(&cd, nullptr, &g.depthCopy);
                if (SUCCEEDED(hr))
                {
                    D3D11_SHADER_RESOURCE_VIEW_DESC vd = {};
                    vd.Format = sf; vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; vd.Texture2D.MipLevels = 1;
                    hr = dev->CreateShaderResourceView(g.depthCopy, &vd, &g.depthSRV);
                }
                if (FAILED(hr)) { Log("depth copy create failed hr=%08x fmt=%d", (unsigned)hr, (int)sd.Format); REL(g.depthSRV); REL(g.depthCopy); }
                else { g.dW = sd.Width; g.dH = sd.Height; g.dFmt = sd.Format; Log("Depth copy %ux%u srcfmt=%d", sd.Width, sd.Height, (int)sd.Format); }
            }
            if (g.depthCopy && g.depthSRV) { ctx->CopyResource(g.depthCopy, best->tex); hasDepth = true; }
        }

        // constants
        D3D11_MAPPED_SUBRESOURCE ms;
        if (FAILED(ctx->Map(g.cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) break;
        float* f = (float*)ms.pData;
        f[0] = g_cfg.sep; f[1] = g_cfg.conv; f[2] = g_cfg.nearZ; f[3] = g_cfg.farZ;
        f[4] = (float)g_cfg.mode; f[5] = (float)g_cfg.reversedZ; f[6] = hasDepth ? 1.f : 0.f; f[7] = (float)g_cfg.type;
        f[8] = (float)g_cfg.swapEyes; f[9] = f[10] = f[11] = 0.f;
        ctx->Unmap(g.cb, 0);

        ID3D11RenderTargetView* rtv = nullptr;
        if (FAILED(dev->CreateRenderTargetView(bb, nullptr, &rtv)) || !rtv) { Log("ERROR RTV on backbuffer"); g.failed = true; break; }

        Saved sv; SaveState(ctx, sv);

        ID3D11ShaderResourceView* srvs[2] = { g.sceneSRV, hasDepth ? g.depthSRV : nullptr };
        ID3D11SamplerState* smps[2] = { g.sLin, g.sPt };
        D3D11_VIEWPORT vp = { 0, 0, (FLOAT)bd.Width, (FLOAT)bd.Height, 0.f, 1.f };
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(g.vs, nullptr, 0);
        ctx->GSSetShader(nullptr, nullptr, 0);
        ctx->HSSetShader(nullptr, nullptr, 0);
        ctx->DSSetShader(nullptr, nullptr, 0);
        ctx->PSSetShader(g.ps, nullptr, 0);
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->PSSetSamplers(0, 2, smps);
        ctx->PSSetConstantBuffers(0, 1, &g.cb);
        ctx->RSSetState(g.rs);
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetBlendState(g.bs, nullptr, 0xffffffff);
        ctx->OMSetDepthStencilState(g.dss, 0);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->Draw(3, 0);

        RestoreState(ctx, sv);
        rtv->Release();
    } while (0);
    REL(bb); REL(ctx); REL(dev);
    g_inside = false;
    ResetCands();
}

static HRESULT STDMETHODCALLTYPE hkPresent(IDXGISwapChain* sc, UINT s, UINT f)
{
    if (!(f & DXGI_PRESENT_TEST)) OnPresent(sc);
    return g_oPresent(sc, s, f);
}
static HRESULT STDMETHODCALLTYPE hkPresent1(IDXGISwapChain1* sc, UINT s, UINT f, const DXGI_PRESENT_PARAMETERS* p)
{
    if (!(f & DXGI_PRESENT_TEST)) OnPresent(sc);
    return g_oPresent1(sc, s, f, p);
}

// ---------------------------------------------------------------- hook install
static void* PatchVtbl(void* obj, int idx, void* hook)
{
    void** vt = *(void***)obj;
    DWORD old = 0;
    if (!VirtualProtect(&vt[idx], sizeof(void*), PAGE_READWRITE, &old))
    { Log("VirtualProtect failed idx=%d err=%lu", idx, GetLastError()); return nullptr; }
    void* orig = vt[idx];
    vt[idx] = hook;
    VirtualProtect(&vt[idx], sizeof(void*), old, &old);
    return orig;
}

static DWORD WINAPI InitThread(LPVOID)
{
    Sleep(4000); // let the game finish its own startup first
    // writable folder: SDL_GetPrefPath of the original SDL2 (WinRT -> LocalState)
    HMODULE orig = GetModuleHandleW(L"SDL2_orig.dll");
    typedef char* (*GetPref)(const char*, const char*);
    GetPref gp = orig ? (GetPref)GetProcAddress(orig, "SDL_GetPrefPath") : nullptr;
    if (gp)
    {
        char* p = gp("VectorUnit", "BBRAnaglyph");
        if (p) { size_t i = 0; while (p[i] && i < sizeof(g_dir) - 1) { g_dir[i] = p[i]; i++; } g_dir[i] = 0; }
    }
    if (g_dir[0])
    {
        char lp[1200]; OpenPath("anaglyph_log.txt", lp, sizeof(lp));
        wchar_t wp[1200]; MultiByteToWideChar(CP_UTF8, 0, lp, -1, wp, 1200);
        g_log = CreateFileW(wp, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        LoadCfg();
    }
    Log("bb_anaglyph started. dir=%s", g_dir);
    Log("step: creating dummy device");

    ID3D11Device* dev = nullptr; ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { Log("dummy device failed %08x", (unsigned)hr); return 0; }

    Log("step: dummy device ok, creating swapchain");
    IDXGIDevice* dxd = nullptr; IDXGIAdapter* ad = nullptr; IDXGIFactory2* fac = nullptr; IDXGISwapChain1* sc = nullptr;
    dev->QueryInterface(IID_IDXGIDevice, (void**)&dxd);
    if (dxd) dxd->GetAdapter(&ad);
    if (ad) ad->GetParent(IID_IDXGIFactory2, (void**)&fac);
    if (!fac) { Log("no IDXGIFactory2"); return 0; }
    DXGI_SWAP_CHAIN_DESC1 d = {};
    d.Width = 64; d.Height = 64; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; d.BufferCount = 2;
    d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL; d.Scaling = DXGI_SCALING_STRETCH; d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    hr = fac->CreateSwapChainForComposition(dev, &d, nullptr, &sc);
    if (FAILED(hr) || !sc) { Log("dummy swapchain failed %08x", (unsigned)hr); return 0; }

    Log("step: dummy swapchain ok, patching vtables");
    g_oPresent     = (PresentFn)PatchVtbl(sc, 8, (void*)hkPresent);
    g_oPresent1    = (Present1Fn)PatchVtbl(sc, 22, (void*)hkPresent1);
    g_oOMSetRT     = (OMSetRTFn)PatchVtbl(ctx, 33, (void*)hkOMSetRT);
    g_oDrawIdx     = (DrawIdxFn)PatchVtbl(ctx, 12, (void*)hkDrawIdx);
    g_oDraw        = (DrawFn)PatchVtbl(ctx, 13, (void*)hkDraw);
    g_oDrawIdxInst = (DrawIdxInstFn)PatchVtbl(ctx, 20, (void*)hkDrawIdxInst);
    g_oDrawInst    = (DrawInstFn)PatchVtbl(ctx, 21, (void*)hkDrawInst);
    Log("Hooks: present=%p present1=%p omset=%p", (void*)g_oPresent, (void*)g_oPresent1, (void*)g_oOMSetRT);

    // note: dummy objects are intentionally leaked; releasing them is harmless but unnecessary
    return 0;
}

// ---------------------------------------------------------------- SDL_PollEvent wrapper (hotkeys)
typedef int (*PollFn)(void*);
extern "C" __declspec(dllexport) int SDL_PollEvent(void* ev)
{
    static PollFn real = nullptr;
    if (!real)
    {
        HMODULE m = GetModuleHandleW(L"SDL2_orig.dll");
        real = m ? (PollFn)GetProcAddress(m, "SDL_PollEvent") : nullptr;
        if (!real) return 0;
    }
    int r = real(ev);
    if (r && ev)
    {
        unsigned type = *(unsigned*)ev;
        if (type == 0x300) // SDL_KEYDOWN
        {
            unsigned char rep = *((unsigned char*)ev + 13);
            int sc = *(int*)((char*)ev + 16);
            if (!rep)
            {
                bool ch = true;
                switch (sc)
                {
                case 62: g_cfg.type = (g_cfg.type + 1) % 3; break;               // F5
                case 63: g_cfg.enabled ^= 1; break;                              // F6
                case 64: g_cfg.mode = (g_cfg.mode + 1) % 3; break;               // F7
                case 65: g_cfg.sep = g_cfg.sep > 0.004f ? g_cfg.sep - 0.002f : g_cfg.sep; break; // F8
                case 66: g_cfg.sep = g_cfg.sep < 0.080f ? g_cfg.sep + 0.002f : g_cfg.sep; break; // F9
                case 67: g_cfg.conv = g_cfg.conv > 1.0f ? g_cfg.conv * 0.85f : g_cfg.conv; break; // F10
                case 68: g_cfg.conv = g_cfg.conv < 500.f ? g_cfg.conv * 1.18f : g_cfg.conv; break; // F11
                case 69: g_cfg.swapEyes ^= 1; break;                             // F12
                case 73: g_cfg.reversedZ ^= 1; break;                            // Insert
                default: ch = false;
                }
                if (ch)
                {
                    Log("cfg: on=%d mode=%d type=%d swap=%d rev=%d sep=%d.%03d conv=%d.%03d", g_cfg.enabled, g_cfg.mode, g_cfg.type, g_cfg.swapEyes, g_cfg.reversedZ, FI(g_cfg.sep), FF(g_cfg.sep), FI(g_cfg.conv), FF(g_cfg.conv));
                    SaveCfg();
                }
            }
        }
    }
    return r;
}

extern "C" BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
