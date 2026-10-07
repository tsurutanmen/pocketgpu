// DirectX 11 の機能を 1 つずつ試す試験の共通部分。窓を作らず、画面の外の的に描いて読み出す。
//   feat.exe <場面> <出す PPM>   値は標準出力に「値 ...」の行で出す（比べる行）。「情報 ...」の行は比べない
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static PFN_COMPILE g_compile;
#define D3D11CalcSubresource(m, s, n) ((UINT) (m) + (UINT) (s) * (UINT) (n))
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;
static ID3D11Device1 *dev1;
static ID3D11DeviceContext1 *ctx1;
static D3D_FEATURE_LEVEL g_fl;
static int g_tw = 256, g_th = 256; // いまの的の大きさ（quad の画素の座標を NDC に直すのに使う）

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("失敗 ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
    exit(3);
}
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) die("%s = 0x%08lx（%d 行）", #x, (unsigned long) hr_, __LINE__); } while (0)

static void val(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("値 ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}
static void info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("情報 ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}

// ---- シェーダー ----
static ID3DBlob *compile(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = g_compile(src, strlen(src), "feat.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) die("シェーダーを組めない（%s %s）：%s", entry, target, err ? (const char *) ID3D10Blob_GetBufferPointer(err) : "?");
    return code;
}
static ID3D11VertexShader *mk_vs(const char *src, const char *e, ID3DBlob **keep) {
    ID3DBlob *b = compile(src, e, "vs_5_0");
    ID3D11VertexShader *s = NULL;
    CK(ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s));
    if (keep) *keep = b;
    return s;
}
static ID3D11PixelShader *mk_ps(const char *src, const char *e) {
    ID3DBlob *b = compile(src, e, "ps_5_0");
    ID3D11PixelShader *s = NULL;
    CK(ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s));
    return s;
}
static ID3D11ComputeShader *mk_cs(const char *src, const char *e) {
    ID3DBlob *b = compile(src, e, "cs_5_0");
    ID3D11ComputeShader *s = NULL;
    CK(ID3D11Device_CreateComputeShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s));
    return s;
}

// ---- 物を作る ----
static ID3D11Buffer *mk_buf(UINT size, D3D11_USAGE usage, UINT bind, UINT cpu, UINT misc, UINT stride, const void *data) {
    D3D11_BUFFER_DESC bd = { .ByteWidth = size, .Usage = usage, .BindFlags = bind, .CPUAccessFlags = cpu, .MiscFlags = misc, .StructureByteStride = stride };
    D3D11_SUBRESOURCE_DATA sd = { data, 0, 0 };
    ID3D11Buffer *b = NULL;
    CK(ID3D11Device_CreateBuffer(dev, &bd, data ? &sd : NULL, &b));
    return b;
}
static ID3D11Buffer *mk_cb(UINT size) { return mk_buf((size + 15) & ~15u, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0, NULL); }

static ID3D11Texture2D *mk_tex(UINT w, UINT h, UINT mips, UINT arr, DXGI_FORMAT f, UINT samples, UINT bind, UINT misc, const D3D11_SUBRESOURCE_DATA *init) {
    D3D11_TEXTURE2D_DESC td = { .Width = w, .Height = h, .MipLevels = mips, .ArraySize = arr, .Format = f, .SampleDesc = { samples ? samples : 1, 0 },
                                .Usage = D3D11_USAGE_DEFAULT, .BindFlags = bind, .MiscFlags = misc };
    ID3D11Texture2D *t = NULL;
    CK(ID3D11Device_CreateTexture2D(dev, &td, init, &t));
    return t;
}
static ID3D11RenderTargetView *mk_rtv(void *res, const D3D11_RENDER_TARGET_VIEW_DESC *d) {
    ID3D11RenderTargetView *v = NULL;
    CK(ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *) res, d, &v));
    return v;
}
static ID3D11DepthStencilView *mk_dsv(void *res, const D3D11_DEPTH_STENCIL_VIEW_DESC *d) {
    ID3D11DepthStencilView *v = NULL;
    CK(ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *) res, d, &v));
    return v;
}
static ID3D11ShaderResourceView *mk_srv(void *res, const D3D11_SHADER_RESOURCE_VIEW_DESC *d) {
    ID3D11ShaderResourceView *v = NULL;
    CK(ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *) res, d, &v));
    return v;
}
static ID3D11UnorderedAccessView *mk_uav(void *res, const D3D11_UNORDERED_ACCESS_VIEW_DESC *d) {
    ID3D11UnorderedAccessView *v = NULL;
    CK(ID3D11Device_CreateUnorderedAccessView(dev, (ID3D11Resource *) res, d, &v));
    return v;
}
static ID3D11SamplerState *mk_samp(D3D11_FILTER f, D3D11_TEXTURE_ADDRESS_MODE a, const float *border, D3D11_COMPARISON_FUNC cmp) {
    D3D11_SAMPLER_DESC sd = { .Filter = f, .AddressU = a, .AddressV = a, .AddressW = a, .MaxAnisotropy = 16, .ComparisonFunc = cmp ? cmp : D3D11_COMPARISON_NEVER,
                              .MinLOD = 0, .MaxLOD = D3D11_FLOAT32_MAX };
    if (border) memcpy(sd.BorderColor, border, 16);
    ID3D11SamplerState *s = NULL;
    CK(ID3D11Device_CreateSamplerState(dev, &sd, &s));
    return s;
}

// 深度とステンシルの状態。sfunc が 0 ならステンシルは使わない
static ID3D11DepthStencilState *mk_dss(BOOL de, BOOL dw, D3D11_COMPARISON_FUNC df, D3D11_COMPARISON_FUNC sfunc, D3D11_STENCIL_OP pass, UINT8 rm, UINT8 wm) {
    D3D11_DEPTH_STENCIL_DESC d = { .DepthEnable = de, .DepthWriteMask = dw ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO, .DepthFunc = df ? df : D3D11_COMPARISON_LESS,
                                   .StencilEnable = sfunc != 0, .StencilReadMask = rm, .StencilWriteMask = wm };
    D3D11_DEPTH_STENCILOP_DESC o = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, pass ? pass : D3D11_STENCIL_OP_KEEP, sfunc ? sfunc : D3D11_COMPARISON_ALWAYS };
    d.FrontFace = o;
    d.BackFace = o;
    ID3D11DepthStencilState *s = NULL;
    CK(ID3D11Device_CreateDepthStencilState(dev, &d, &s));
    return s;
}
static ID3D11BlendState *mk_bs1(BOOL en, D3D11_BLEND s, D3D11_BLEND d, D3D11_BLEND_OP op, D3D11_BLEND sa, D3D11_BLEND da, D3D11_BLEND_OP opa, UINT8 mask) {
    D3D11_BLEND_DESC bd = { 0 };
    for (int i = 0; i < 8; i++) {
        D3D11_RENDER_TARGET_BLEND_DESC r = { en, s ? s : D3D11_BLEND_ONE, d ? d : D3D11_BLEND_ZERO, op ? op : D3D11_BLEND_OP_ADD,
                                             sa ? sa : D3D11_BLEND_ONE, da ? da : D3D11_BLEND_ZERO, opa ? opa : D3D11_BLEND_OP_ADD, mask };
        bd.RenderTarget[i] = r;
    }
    ID3D11BlendState *b = NULL;
    CK(ID3D11Device_CreateBlendState(dev, &bd, &b));
    return b;
}
static ID3D11BlendState *bs_off(void) { return mk_bs1(FALSE, 0, 0, 0, 0, 0, 0, 15); }
static ID3D11RasterizerState *mk_rs(D3D11_FILL_MODE fill, D3D11_CULL_MODE cull, BOOL fccw, INT bias, float clamp, float slope, BOOL clip, BOOL scissor, BOOL ms) {
    D3D11_RASTERIZER_DESC rd = { .FillMode = fill ? fill : D3D11_FILL_SOLID, .CullMode = cull ? cull : D3D11_CULL_NONE, .FrontCounterClockwise = fccw,
                                 .DepthBias = bias, .DepthBiasClamp = clamp, .SlopeScaledDepthBias = slope, .DepthClipEnable = clip,
                                 .ScissorEnable = scissor, .MultisampleEnable = ms };
    ID3D11RasterizerState *r = NULL;
    CK(ID3D11Device_CreateRasterizerState(dev, &rd, &r));
    return r;
}
static ID3D11RasterizerState *rs_def(void) { return mk_rs(0, 0, FALSE, 0, 0, 0, TRUE, FALSE, FALSE); }

static void viewport(float x, float y, float w, float h, float mn, float mx) {
    D3D11_VIEWPORT vp = { x, y, w, h, mn, mx };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
}
// 的をつなぐ（的の大きさも覚える）
static void targets(UINT n, ID3D11RenderTargetView *const *rtv, ID3D11DepthStencilView *dsv, int w, int h) {
    ID3D11DeviceContext_OMSetRenderTargets(ctx, n, rtv, dsv);
    g_tw = w;
    g_th = h;
    viewport(0, 0, (float) w, (float) h, 0, 1);
    ID3D11DeviceContext_RSSetState(ctx, rs_def());
    float bf[4] = { 0 };
    ID3D11DeviceContext_OMSetBlendState(ctx, bs_off(), bf, 0xffffffff);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, mk_dss(FALSE, FALSE, 0, 0, 0, 0xff, 0xff), 0);
}
static void clear_rtv(ID3D11RenderTargetView *v, float r, float g, float b, float a) {
    float c[4] = { r, g, b, a };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, v, c);
}

// ---- 四角と三角を描く共通のシェーダー ----
// Q：rect は NDC の左上と右下、zz.x と zz.y は左端と右端の深度、ex.x は塗り方
#define Q_HEAD \
    "cbuffer Q : register(b0) { float4 rect; float4 col; float4 zz; float4 ex; };\n" \
    "struct VO { float4 pos : SV_Position; float4 col : COLOR; float2 uv : UV; };\n"
static const char *SRC_Q = Q_HEAD
    "VO vsq(uint id : SV_VertexID) { float2 t = float2(id & 1, id >> 1); VO o;\n"
    "  o.pos = float4(lerp(rect.xy, rect.zw, t), lerp(zz.x, zz.y, t.x), 1); o.col = col; o.uv = t; return o; }\n"
    "float4 shade(VO i) { float4 c = i.col;\n"
    "  if (ex.x == 1) c.a *= i.uv.y;\n"
    "  if (ex.x == 2) c.rgb *= float3(i.uv.x, i.uv.y, 1 - i.uv.x);\n"
    "  if (ex.x == 3) { float2 d = i.uv - 0.5; if (dot(d, d) > 0.2) discard; }\n"
    "  if (ex.x == 4) c = float4(i.uv.x, i.uv.y, c.b, c.a);\n"
    "  if (ex.x == 5) c.rgb = 1 - c.rgb;\n"
    "  return c; }\n"
    "float4 psq(VO i) : SV_Target { return shade(i); }\n"
    // 三角：位置を cbuffer の 3 つから取る（xyz と色）
    "cbuffer T : register(b1) { float4 tp[3]; float4 tcol; };\n"
    "VO vst(uint id : SV_VertexID) { VO o; o.pos = float4(tp[id].xyz, 1); o.col = tcol; o.uv = float2(id == 1, id == 2); return o; }\n";
static ID3D11VertexShader *g_vsq, *g_vst;
static ID3D11PixelShader *g_psq;
static ID3D11Buffer *g_cbq, *g_cbt;

static void q_init(void) {
    if (g_vsq) return;
    g_vsq = mk_vs(SRC_Q, "vsq", NULL);
    g_vst = mk_vs(SRC_Q, "vst", NULL);
    g_psq = mk_ps(SRC_Q, "psq");
    g_cbq = mk_cb(64);
    g_cbt = mk_cb(64);
}
// 四角を描く共通の物をつなぐ。ps が NULL なら psq
static void q_use(ID3D11PixelShader *ps) {
    q_init();
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, g_vsq, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps ? ps : g_psq, NULL, 0);
    ID3D11Buffer *b[2] = { g_cbq, g_cbt };
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 2, b);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 0, 2, b);
}
static float nx(float x) { return x / (float) g_tw * 2.0f - 1.0f; }
static float ny(float y) { return 1.0f - y / (float) g_th * 2.0f; }
// 画素の座標の四角（x0,y0 が左上）。z0 は左端・z1 は右端の深度。mode は ex.x、e1〜e3 は ex.yzw
static void quad4(float x0, float y0, float x1, float y1, float r, float g, float b, float a, float z0, float z1, float mode, float e1, float e2, float e3) {
    float cb[16] = { nx(x0), ny(y0), nx(x1), ny(y1), r, g, b, a, z0, z1, 0, 0, mode, e1, e2, e3 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, g_vsq, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 4, 0);
}
static void quad(float x0, float y0, float x1, float y1, float r, float g, float b, float a, float z0, float z1, float mode) {
    quad4(x0, y0, x1, y1, r, g, b, a, z0, z1, mode, 0, 0, 0);
}
// 画素の座標の三角（z は頂点ごと）
static void tri(float x0, float y0, float z0, float x1, float y1, float z1, float x2, float y2, float z2, float r, float g, float b, float a) {
    float cb[16] = { nx(x0), ny(y0), z0, 0, nx(x1), ny(y1), z1, 0, nx(x2), ny(y2), z2, 0, r, g, b, a };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbt, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, g_vst, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
}
// 線と点（三角と同じ cbuffer の頂点を使う）
static void prim2(D3D11_PRIMITIVE_TOPOLOGY top, int n, const float *xy, float r, float g, float b) {
    float cb[16] = { 0 };
    for (int i = 0; i < n && i < 3; i++) { cb[i * 4] = nx(xy[i * 2]); cb[i * 4 + 1] = ny(xy[i * 2 + 1]); cb[i * 4 + 2] = 0.5f; }
    cb[12] = r; cb[13] = g; cb[14] = b; cb[15] = 1;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbt, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, top);
    ID3D11DeviceContext_VSSetShader(ctx, g_vst, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, (UINT) n, 0);
}

// ---- 形式を読む ----
static float h2f(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v;
    if (e == 0) v = ldexpf((float) m, -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = ldexpf((float) (m | 1024), (int) e - 25);
    return s ? -v : v;
}
static uint16_t f2h(float f) {
    uint32_t b;
    memcpy(&b, &f, 4);
    uint32_t sign = (b >> 16) & 0x8000;
    int e = (int) ((b >> 23) & 0xff) - 127 + 15;
    uint32_t m = b & 0x7fffff;
    if (((b >> 23) & 0xff) == 0 || e <= 0) return (uint16_t) sign;
    if (e >= 31) return (uint16_t) (sign | 0x7c00);
    uint32_t r = sign | (uint32_t) e << 10 | (m >> 13);
    if (m & 0x1000) r++; // 四捨五入（だいたい）
    return (uint16_t) r;
}
static float f11(uint32_t x) { uint32_t e = (x >> 6) & 31, m = x & 63; if (e == 0) return ldexpf((float) m, -20); if (e == 31) return INFINITY; return ldexpf((float) (m | 64), (int) e - 21); }
static float f10(uint32_t x) { uint32_t e = (x >> 5) & 31, m = x & 31; if (e == 0) return ldexpf((float) m, -19); if (e == 31) return INFINITY; return ldexpf((float) (m | 32), (int) e - 20); }

static int fmt_bpp(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_UNORM: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_R32G32_UINT: return 8;
    case DXGI_FORMAT_R16G16_FLOAT: case DXGI_FORMAT_R16G16_UNORM: return 4;
    case DXGI_FORMAT_R8_UNORM: return 1;
    case DXGI_FORMAT_R16_FLOAT: case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_R8G8_UNORM: return 2;
    default: return 4;
    }
}
static void decode(DXGI_FORMAT f, const uint8_t *p, float *o) {
    uint32_t v;
    memcpy(&v, p, 4);
    o[3] = 1;
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_SNORM:
        for (int i = 0; i < 4; i++) { float x = (int8_t) p[i] / 127.0f; o[i] = x < -1 ? -1 : x; }
        break;
    case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        o[0] = p[2] / 255.0f; o[1] = p[1] / 255.0f; o[2] = p[0] / 255.0f; o[3] = p[3] / 255.0f;
        break;
    case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_UINT: case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        o[0] = (v & 1023) / 1023.0f; o[1] = ((v >> 10) & 1023) / 1023.0f; o[2] = ((v >> 20) & 1023) / 1023.0f; o[3] = (v >> 30) / 3.0f;
        break;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + i * 2, 2); o[i] = h2f(h); }
        break;
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        for (int i = 0; i < 4; i++) { uint16_t h; memcpy(&h, p + i * 2, 2); o[i] = h / 65535.0f; }
        break;
    case DXGI_FORMAT_R11G11B10_FLOAT:
        o[0] = f11(v & 2047); o[1] = f11((v >> 11) & 2047); o[2] = f10(v >> 22);
        break;
    case DXGI_FORMAT_R16G16_FLOAT: {
        uint16_t a, b;
        memcpy(&a, p, 2); memcpy(&b, p + 2, 2);
        o[0] = h2f(a); o[1] = h2f(b); o[2] = 0;
        break;
    }
    case DXGI_FORMAT_R16G16_UNORM: {
        uint16_t a, b;
        memcpy(&a, p, 2); memcpy(&b, p + 2, 2);
        o[0] = a / 65535.0f; o[1] = b / 65535.0f; o[2] = 0;
        break;
    }
    case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: {
        float x;
        memcpy(&x, p, 4);
        o[0] = o[1] = o[2] = x;
        break;
    }
    case DXGI_FORMAT_R32_UINT:
        o[0] = (v & 255) / 255.0f; o[1] = ((v >> 8) & 255) / 255.0f; o[2] = ((v >> 16) & 255) / 255.0f;
        break;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        memcpy(o, p, 16);
        break;
    case DXGI_FORMAT_R8_UNORM:
        o[0] = o[1] = o[2] = p[0] / 255.0f;
        break;
    case DXGI_FORMAT_R8G8_UNORM:
        o[0] = p[0] / 255.0f; o[1] = p[1] / 255.0f; o[2] = 0;
        break;
    case DXGI_FORMAT_R16_FLOAT: {
        uint16_t h;
        memcpy(&h, p, 2);
        o[0] = o[1] = o[2] = h2f(h);
        break;
    }
    case DXGI_FORMAT_R16_UNORM: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_TYPELESS: {
        uint16_t h;
        memcpy(&h, p, 2);
        o[0] = o[1] = o[2] = h / 65535.0f;
        break;
    }
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS:
        o[0] = o[1] = o[2] = (v & 0xffffff) / 16777215.0f; o[3] = (v >> 24) / 255.0f;
        break;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: {
        float x;
        memcpy(&x, p, 4);
        o[0] = o[1] = o[2] = x; o[3] = p[4] / 255.0f;
        break;
    }
    default: // RGBA8 の仲間（UNORM・SRGB・UINT・TYPELESS）は、入っているバイトのまま
        for (int i = 0; i < 4; i++) o[i] = p[i] / 255.0f;
    }
}

// 読み出した絵（詰めた並び）
typedef struct { int w, h, d, bpp; DXGI_FORMAT f; uint8_t *px; } img;
static void img_get(const img *m, int x, int y, float *o) { decode(m->f, m->px + ((size_t) y * m->w + x) * m->bpp, o); }
static uint32_t img_u32(const img *m, int x, int y) { uint32_t v; memcpy(&v, m->px + ((size_t) y * m->w + x) * m->bpp, 4); return v; }
static uint32_t fnv(const void *p, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *) p)[i]) * 16777619u;
    return h;
}

// 2D のテクスチャの 1 つの部分（mip・層）をステージングに写して読む。as は読み分けの形式（0 ならテクスチャの形式）
static img read_tex(ID3D11Texture2D *t, UINT mip, UINT slice, DXGI_FORMAT as) {
    D3D11_TEXTURE2D_DESC d;
    ID3D11Texture2D_GetDesc(t, &d);
    img m = { .w = (int) (d.Width >> mip ? d.Width >> mip : 1), .h = (int) (d.Height >> mip ? d.Height >> mip : 1), .d = 1, .f = as ? as : d.Format };
    m.bpp = fmt_bpp(m.f);
    D3D11_TEXTURE2D_DESC sd = { .Width = (UINT) m.w, .Height = (UINT) m.h, .MipLevels = 1, .ArraySize = 1, .Format = d.Format, .SampleDesc = { 1, 0 },
                                .Usage = D3D11_USAGE_STAGING, .CPUAccessFlags = D3D11_CPU_ACCESS_READ };
    ID3D11Texture2D *st = NULL;
    CK(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &st));
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *) st, 0, 0, 0, 0, (ID3D11Resource *) t, D3D11CalcSubresource(mip, slice, d.MipLevels), NULL);
    D3D11_MAPPED_SUBRESOURCE mp;
    CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) st, 0, D3D11_MAP_READ, 0, &mp));
    m.px = malloc((size_t) m.w * m.h * m.bpp);
    for (int y = 0; y < m.h; y++) memcpy(m.px + (size_t) y * m.w * m.bpp, (const uint8_t *) mp.pData + (size_t) y * mp.RowPitch, (size_t) m.w * m.bpp);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) st, 0);
    ID3D11Texture2D_Release(st);
    return m;
}
static void *read_buf(ID3D11Buffer *b, UINT size) {
    ID3D11Buffer *st = mk_buf(size, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0, NULL);
    D3D11_BOX box = { 0, 0, 0, size, 1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *) st, 0, 0, 0, 0, (ID3D11Resource *) b, 0, &box);
    D3D11_MAPPED_SUBRESOURCE mp;
    CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) st, 0, D3D11_MAP_READ, 0, &mp));
    void *p = malloc(size);
    memcpy(p, mp.pData, size);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) st, 0);
    ID3D11Buffer_Release(st);
    return p;
}

// ---- 絵を並べて PPM に出す ----
typedef struct { int w, h; uint8_t *rgb; char label[80]; } panel;
static panel g_pan[96];
static int g_npan;
static uint8_t to8(float v) { if (!(v > 0)) return 0; if (v >= 1) return 255; return (uint8_t) (v * 255.0f + 0.5f); }
// 読み出した絵を 1 枚の絵として足す。alpha が 1 なら、アルファを灰色にした絵も右に足す
static void add_img(const img *m, int alpha, const char *label) {
    for (int k = 0; k <= alpha; k++) {
        if (g_npan >= 96) return;
        panel *p = &g_pan[g_npan++];
        p->w = m->w; p->h = m->h;
        p->rgb = malloc((size_t) m->w * m->h * 3);
        snprintf(p->label, sizeof p->label, "%s%s", label, k ? "・アルファ" : "");
        for (int y = 0; y < m->h; y++)
            for (int x = 0; x < m->w; x++) {
                float o[4];
                img_get(m, x, y, o);
                uint8_t *q = p->rgb + ((size_t) y * m->w + x) * 3;
                if (k) q[0] = q[1] = q[2] = to8(o[3]);
                else { q[0] = to8(o[0]); q[1] = to8(o[1]); q[2] = to8(o[2]); }
            }
        info("絵 %s %dx%d 形式 %d 中身の検算 %08x", p->label, m->w, m->h, (int) m->f, fnv(m->px, (size_t) m->w * m->h * m->bpp));
    }
}
static img show(ID3D11Texture2D *t, UINT mip, UINT slice, DXGI_FORMAT as, int alpha, const char *label) {
    img m = read_tex(t, mip, slice, as);
    add_img(&m, alpha, label);
    return m;
}
// 画素の値を「値」の行で出す
static void val_px(const img *m, int x, int y, const char *label) {
    float o[4];
    img_get(m, x, y, o);
    val("%s (%d,%d) = %.4f %.4f %.4f %.4f", label, x, y, o[0], o[1], o[2], o[3]);
}
static void save_ppm(const char *path) {
    enum { MAXW = 1024, GAP = 2 };
    int x = 0, y = 0, rowh = 0, W = 0, H = 0;
    int *px = calloc(g_npan + 1, sizeof(int)), *py = calloc(g_npan + 1, sizeof(int));
    for (int i = 0; i < g_npan; i++) {
        if (x > 0 && x + g_pan[i].w > MAXW) { x = 0; y += rowh + GAP; rowh = 0; }
        px[i] = x; py[i] = y;
        x += g_pan[i].w + GAP;
        if (g_pan[i].h > rowh) rowh = g_pan[i].h;
        if (px[i] + g_pan[i].w > W) W = px[i] + g_pan[i].w;
        if (py[i] + g_pan[i].h > H) H = py[i] + g_pan[i].h;
    }
    if (!g_npan) { W = H = 1; }
    uint8_t *o = calloc((size_t) W * H, 3);
    for (int i = 0; i < g_npan; i++)
        for (int yy = 0; yy < g_pan[i].h; yy++)
            memcpy(o + ((size_t) (py[i] + yy) * W + px[i]) * 3, g_pan[i].rgb + (size_t) yy * g_pan[i].w * 3, (size_t) g_pan[i].w * 3);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        fwrite(o, 3, (size_t) W * H, f);
        fclose(f);
    }
    info("PPM %d x %d に %d 枚", W, H, g_npan);
    free(o); free(px); free(py);
}

// クエリの答えを待つ（10 秒で諦める）。1 で取れた・0 で時間切れ・-1 で失敗
static int get_data(void *q, void *data, UINT size) {
    DWORD t0 = GetTickCount();
    for (;;) {
        HRESULT hr = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *) q, data, size, 0);
        if (hr == S_OK) return 1;
        if (FAILED(hr)) return -1;
        if (GetTickCount() - t0 > 10000) return 0;
        Sleep(0);
    }
}
