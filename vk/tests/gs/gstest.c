// geometry shader の真似を確かめる DirectX 11 の試験。窓を作らず、絵の外の的（render target）に描いて読む。
//   gstest.exe <場面> <出す PPM>
// 場面：
//   pidx    点を受ける GS・番号つき（16 ビットと 32 ビットの index・StartIndex・BaseVertex）。比べ用に番号なしも 1 回
//   pinst   点を受ける GS・インスタンスつき（インスタンスごとの頂点の属性・SV_InstanceID）。番号つき＋インスタンスつきも
//   tri     三角を受ける GS・番号つきの立方体。面の向きで色を付け、少し外へ広げる（maxvertexcount 3）
//   cut     三角を受ける GS・1 つの三角から 2 つの三角を出し、間で RestartStrip（maxvertexcount 6）。三角の帯の入力と線の入力も
//   layer   三角を受ける GS・SV_RenderTargetArrayIndex で 4 枚の層に描く（ループと RestartStrip・maxvertexcount 12）。
//           右半分は [instance(4)] で同じことをする。4 枚を 2x2 に並べて出す
//   adj     隣を持つ三角（triangleadj）を受ける GS・輪郭の線を太い四角で出す
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 256, H = 256 };

typedef HRESULT (WINAPI *PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static PFN_COMPILE g_compile;
static ID3D11Device *dev;
static ID3D11DeviceContext *ctx;

static ID3DBlob *compile(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = g_compile(src, strlen(src), "gstest.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        printf("シェーダーを組めない（%s）：%s\n", entry, err ? (const char *) ID3D10Blob_GetBufferPointer(err) : "?");
        exit(1);
    }
    return code;
}

static ID3D11VertexShader *mk_vs(const char *src, const char *e, ID3DBlob **keep) {
    ID3DBlob *b = compile(src, e, "vs_5_0");
    ID3D11VertexShader *s = NULL;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s);
    if (keep) *keep = b;
    return s;
}
static ID3D11GeometryShader *mk_gs(const char *src, const char *e) {
    ID3DBlob *b = compile(src, e, "gs_5_0");
    ID3D11GeometryShader *s = NULL;
    if (FAILED(ID3D11Device_CreateGeometryShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s))) { printf("GS を作れない\n"); exit(1); }
    return s;
}
static ID3D11PixelShader *mk_ps(const char *src, const char *e) {
    ID3DBlob *b = compile(src, e, "ps_5_0");
    ID3D11PixelShader *s = NULL;
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), NULL, &s);
    return s;
}
static ID3D11Buffer *mk_buf(const void *data, UINT size, UINT bind) {
    D3D11_BUFFER_DESC bd = { .ByteWidth = size, .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = bind };
    D3D11_SUBRESOURCE_DATA sd = { data, 0, 0 };
    ID3D11Buffer *b = NULL;
    if (FAILED(ID3D11Device_CreateBuffer(dev, &bd, &sd, &b))) { printf("buffer を作れない\n"); exit(1); }
    return b;
}
static ID3D11Buffer *mk_cb(const void *data, UINT size) {
    D3D11_BUFFER_DESC bd = { .ByteWidth = (size + 15) & ~15u, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_CONSTANT_BUFFER };
    void *tmp = calloc(1, bd.ByteWidth);
    memcpy(tmp, data, size);
    D3D11_SUBRESOURCE_DATA sd = { tmp, 0, 0 };
    ID3D11Buffer *b = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, &sd, &b);
    free(tmp);
    return b;
}

// 描く先：layers 枚の層を持つ RGBA8 の的
typedef struct { ID3D11Texture2D *tex, *st; ID3D11RenderTargetView *rtv; ID3D11Texture2D *dtex; ID3D11DepthStencilView *dsv; int layers; } target;
static target mk_target(int layers, int depth) {
    target t = { .layers = layers };
    D3D11_TEXTURE2D_DESC td = { .Width = W, .Height = H, .MipLevels = 1, .ArraySize = layers, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_RENDER_TARGET };
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &t.tex);
    D3D11_RENDER_TARGET_VIEW_DESC rd = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM };
    if (layers > 1) {
        rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        rd.Texture2DArray.ArraySize = layers;
    } else rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *) t.tex, &rd, &t.rtv);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &t.st);
    if (depth) {
        D3D11_TEXTURE2D_DESC dd = { .Width = W, .Height = H, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_D24_UNORM_S8_UINT,
                                    .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_DEPTH_STENCIL };
        ID3D11Device_CreateTexture2D(dev, &dd, NULL, &t.dtex);
        ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *) t.dtex, NULL, &t.dsv);
    }
    return t;
}
static void begin(target *t, float r, float g, float b) {
    float clear[4] = { r, g, b, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(ctx, t->rtv, clear);
    if (t->dsv) ID3D11DeviceContext_ClearDepthStencilView(ctx, t->dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &t->rtv, t->dsv);
    D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    D3D11_RASTERIZER_DESC rd = { .FillMode = D3D11_FILL_SOLID, .CullMode = D3D11_CULL_NONE, .DepthClipEnable = TRUE };
    ID3D11RasterizerState *rs = NULL;
    ID3D11Device_CreateRasterizerState(dev, &rd, &rs);
    ID3D11DeviceContext_RSSetState(ctx, rs);
}

// 的を読んで PPM に書く。層が 4 枚なら 2x2 に並べる
static int finish(target *t, const char *out) {
    int cols = t->layers > 1 ? 2 : 1, rows = (t->layers + cols - 1) / cols;
    int ow = W * cols, oh = H * rows;
    uint8_t *img = calloc((size_t) ow * oh, 3);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *) t->st, (ID3D11Resource *) t->tex);
    uint32_t colored = 0, sum = 2166136261u;
    for (int l = 0; l < t->layers; l++) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) t->st, (UINT) l, D3D11_MAP_READ, 0, &m))) { printf("絵を読めない\n"); return 1; }
        int ox = (l % cols) * W, oy = (l / cols) * H;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint8_t *p = (const uint8_t *) m.pData + (size_t) y * m.RowPitch + (size_t) x * 4;
                uint8_t *q = img + ((size_t) (oy + y) * ow + ox + x) * 3;
                q[0] = p[0]; q[1] = p[1]; q[2] = p[2];
                if (p[0] > 60 || p[1] > 60 || p[2] > 60) colored++;
                for (int k = 0; k < 3; k++) sum = (sum ^ p[k]) * 16777619u;
            }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) t->st, (UINT) l);
    }
    printf("絵 %d x %d：色のついた画素 %u 個・検算 %08x\n", ow, oh, colored, sum);
    FILE *f = fopen(out, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", ow, oh);
        fwrite(img, 3, (size_t) ow * oh, f);
        fclose(f);
    }
    free(img);
    return 0;
}

// ---- 点を受ける GS ----
static const char *SRC_P =
    "struct VI { float2 pos : POSITION; float4 col : COLOR; float3 inst : INST; };\n"
    "struct VO { float4 pos : POS; float4 col : COLOR; uint vid : VID; uint iid : IID; };\n"
    "VO vs(VI i, uint vid : SV_VertexID, uint iid : SV_InstanceID) {\n"
    "  VO o; o.pos = float4(i.pos + i.inst.xy, 0, 1); o.col = i.col * i.inst.z; o.vid = vid; o.iid = iid; return o; }\n"
    "struct GO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "[maxvertexcount(4)] void gs(point VO p[1], uint prim : SV_PrimitiveID, inout TriangleStream<GO> s) {\n"
    "  float r = 0.025 + 0.012 * (prim % 3) + 0.004 * (p[0].iid % 2);\n"
    "  float4 c = p[0].col; c.b = saturate(c.b + 0.08 * (p[0].vid % 4));\n"
    "  GO o; o.col = c;\n"
    "  o.pos = p[0].pos + float4(-r, -r, 0, 0); s.Append(o);\n"
    "  o.pos = p[0].pos + float4( r, -r, 0, 0); s.Append(o);\n"
    "  o.pos = p[0].pos + float4(-r,  r, 0, 0); s.Append(o);\n"
    "  o.pos = p[0].pos + float4( r,  r, 0, 0); s.Append(o);\n"
    "  s.RestartStrip(); }\n"
    "float4 ps(GO i) : SV_Target { return i.col; }\n";

struct pv { float x, y; uint8_t c[4]; };
struct pinst { float dx, dy, k; };

static void scene_points(int inst_scene, const char *out) {
    ID3DBlob *vsb = NULL;
    ID3D11VertexShader *vs = mk_vs(SRC_P, "vs", &vsb);
    ID3D11GeometryShader *gs = mk_gs(SRC_P, "gs");
    ID3D11PixelShader *ps = mk_ps(SRC_P, "ps");
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "INST", 0, DXGI_FORMAT_R32G32B32_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
    };
    ID3D11InputLayout *lay = NULL;
    if (FAILED(ID3D11Device_CreateInputLayout(dev, il, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay))) { printf("頂点の並びを作れない\n"); exit(1); }
    // 8x8 の格子の点。色は場所で変える
    struct pv v[64];
    for (int i = 0; i < 64; i++) {
        int gx = i % 8, gy = i / 8;
        v[i].x = -0.8f + gx * 0.2286f;
        v[i].y = -0.8f + gy * 0.2286f;
        v[i].c[0] = (uint8_t) (40 + gx * 30);
        v[i].c[1] = (uint8_t) (40 + gy * 30);
        v[i].c[2] = (uint8_t) ((i * 37) & 0x7f);
        v[i].c[3] = 255;
    }
    uint16_t ix16[48];
    uint32_t ix32[48];
    for (int i = 0; i < 48; i++) { ix16[i] = (uint16_t) ((i * 5 + 3) % 61); ix32[i] = (uint32_t) ((i * 7 + 1) % 60); }
    // インスタンスの属性：ずらしと明るさ
    struct pinst in[4] = { { 0, 0, 1.0f }, { 0.05f, 0.02f, 0.6f }, { -0.04f, 0.06f, 0.8f }, { 0.02f, -0.07f, 0.45f } };
    ID3D11Buffer *vb = mk_buf(v, sizeof v, D3D11_BIND_VERTEX_BUFFER), *ib16 = mk_buf(ix16, sizeof ix16, D3D11_BIND_INDEX_BUFFER),
                 *ib32 = mk_buf(ix32, sizeof ix32, D3D11_BIND_INDEX_BUFFER), *ivb = mk_buf(in, sizeof in, D3D11_BIND_VERTEX_BUFFER);
    target t = mk_target(1, 0);
    begin(&t, 0.05f, 0.05f, 0.08f);
    ID3D11DeviceContext_IASetInputLayout(ctx, lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    UINT strides[2] = { sizeof(struct pv), sizeof(struct pinst) }, offs[2] = { 0, 0 };
    ID3D11Buffer *vbs[2] = { vb, ivb };
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 2, vbs, strides, offs);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    if (!inst_scene) {
        // 番号なし（いまの B の道）：左下の 12 点
        ID3D11DeviceContext_Draw(ctx, 12, 0);
        // 16 ビットの index：StartIndex 5・BaseVertex 0
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib16, DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_DrawIndexed(ctx, 20, 5, 0);
        // 32 ビットの index・buffer の中の 8 バイト先から・StartIndex 2・BaseVertex 3
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib32, DXGI_FORMAT_R32_UINT, 8);
        ID3D11DeviceContext_DrawIndexed(ctx, 15, 2, 3);
    } else {
        // インスタンスつき（番号なし）：16 点 x 3 インスタンス・StartVertex 4・StartInstance 1
        ID3D11DeviceContext_DrawInstanced(ctx, 16, 3, 4, 1);
        // 番号つき＋インスタンスつき：10 点 x 2・16 ビット・StartIndex 30・BaseVertex 2・StartInstance 0
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib16, DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_DrawIndexedInstanced(ctx, 10, 2, 30, 2, 0);
    }
    finish(&t, out);
}

// ---- 三角を受ける GS（立方体）----
static const char *SRC_T =
    "cbuffer C : register(b0) { row_major float4x4 mvp; float4 k; };\n"
    "struct VI { float3 pos : POSITION; float2 uv : TEXCOORD; };\n"
    "struct VO { float3 wpos : WPOS; float2 uv : TEXCOORD; };\n"
    "VO vs(VI i) { VO o; o.wpos = i.pos; o.uv = i.uv; return o; }\n"
    "struct GO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "[maxvertexcount(3)] void gs(triangle VO t[3], uint prim : SV_PrimitiveID, inout TriangleStream<GO> s) {\n"
    "  float3 n = normalize(cross(t[1].wpos - t[0].wpos, t[2].wpos - t[0].wpos));\n"
    "  float3 c = abs(n) * 0.7 + 0.15 * float3(prim % 2, (prim / 2) % 2, (prim / 4) % 2);\n"
    "  for (int v = 0; v < 3; v++) { GO o; o.pos = mul(mvp, float4(t[v].wpos + n * k.x, 1)); o.col = float4(c * (0.6 + 0.4 * t[v].uv.x), 1); s.Append(o); }\n"
    "}\n"
    "float4 ps(GO i) : SV_Target { return i.col; }\n";

struct cv { float x, y, z, u, v; };

static void make_cube(struct cv *v, uint16_t *ix) {
    static const float n[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    for (int f = 0; f < 6; f++) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        float ax = ny != 0 ? 1 : 0, ay = 0, az = ny != 0 ? 0 : (nx != 0 ? 1 : 0);
        if (nz != 0) { ax = 1; az = 0; }
        float bx = ny * az - nz * ay, by = nz * ax - nx * az, bz = nx * ay - ny * ax;
        static const float c[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        for (int k = 0; k < 4; k++) {
            struct cv *p = &v[f * 4 + k];
            p->x = nx + c[k][0] * ax + c[k][1] * bx;
            p->y = ny + c[k][0] * ay + c[k][1] * by;
            p->z = nz + c[k][0] * az + c[k][1] * bz;
            p->u = (c[k][0] + 1) * 0.5f;
            p->v = (c[k][1] + 1) * 0.5f;
        }
        static const uint16_t t[6] = { 0, 1, 2, 0, 2, 3 };
        for (int k = 0; k < 6; k++) ix[f * 6 + k] = (uint16_t) (f * 4 + t[k]);
    }
}

static void mat_mul(float *o, const float *a, const float *b) {
    float r[16];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[i * 4 + k] * b[k * 4 + j];
            r[i * 4 + j] = s;
        }
    memcpy(o, r, sizeof r);
}
static void make_mvp(float *m, float t, float dist) {
    float cy = cosf(t), sy = sinf(t), cx = cosf(t * 0.7f), sx = sinf(t * 0.7f);
    float ry[16] = { cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0, 0, 0, 0, 1 };
    float rx[16] = { 1, 0, 0, 0, 0, cx, -sx, 0, 0, sx, cx, 0, 0, 0, 0, 1 };
    float tr[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, dist, 0, 0, 0, 1 };
    float zn = 0.1f, zf = 100.0f, f = 1.0f / tanf(0.5f * 1.0f);
    float pr[16] = { f, 0, 0, 0, 0, f, 0, 0, 0, 0, zf / (zf - zn), -zn * zf / (zf - zn), 0, 0, 1, 0 };
    float r[16];
    mat_mul(r, ry, rx);
    mat_mul(r, tr, r);
    mat_mul(m, pr, r);
}

static void scene_tri(const char *out) {
    ID3DBlob *vsb = NULL;
    ID3D11VertexShader *vs = mk_vs(SRC_T, "vs", &vsb);
    ID3D11GeometryShader *gs = mk_gs(SRC_T, "gs");
    ID3D11PixelShader *ps = mk_ps(SRC_T, "ps");
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *lay = NULL;
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay);
    struct cv v[24];
    uint16_t ix[36];
    make_cube(v, ix);
    ID3D11Buffer *vb = mk_buf(v, sizeof v, D3D11_BIND_VERTEX_BUFFER), *ib = mk_buf(ix, sizeof ix, D3D11_BIND_INDEX_BUFFER);
    float cb[20];
    make_mvp(cb, 0.6f, 5.0f);
    cb[16] = 0.25f; cb[17] = cb[18] = cb[19] = 0;
    ID3D11Buffer *cbuf = mk_cb(cb, sizeof cb);
    target t = mk_target(1, 1);
    begin(&t, 0.06f, 0.06f, 0.06f);
    ID3D11DeviceContext_IASetInputLayout(ctx, lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(struct cv), off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_GSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 36, 0, 0);
    finish(&t, out);
}

// ---- 三角を受けて、間で RestartStrip を挟んで 2 つ出す GS ----
static const char *SRC_C =
    "struct VI { float2 pos : POSITION; float4 col : COLOR; };\n"
    "struct VO { float2 pos : POS; float4 col : COLOR; };\n"
    "VO vs(VI i) { VO o; o.pos = i.pos; o.col = i.col; return o; }\n"
    "struct GO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "[maxvertexcount(6)] void gs(triangle VO t[3], uint prim : SV_PrimitiveID, inout TriangleStream<GO> s) {\n"
    "  float2 m = (t[0].pos + t[1].pos + t[2].pos) / 3;\n"
    "  for (int v = 0; v < 3; v++) { GO o; o.pos = float4(lerp(m, t[v].pos, 0.8), 0, 1); o.col = t[v].col; s.Append(o); }\n"
    "  s.RestartStrip();\n"
    "  for (int w = 0; w < 3; w++) { GO o; o.pos = float4(lerp(m, t[w].pos, 0.3) + float2(0.02, -0.03), 0, 1);\n"
    "    o.col = float4(1 - t[w].col.rgb, 1) * (0.5 + 0.1 * (prim % 5)); s.Append(o); }\n"
    "}\n"
    "struct LO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "[maxvertexcount(4)] void gsl(line VO l[2], inout TriangleStream<LO> s) {\n"
    "  float2 d = normalize(l[1].pos - l[0].pos), n = float2(-d.y, d.x) * 0.015;\n"
    "  LO o; o.col = l[0].col;\n"
    "  o.pos = float4(l[0].pos - n, 0, 1); s.Append(o); o.pos = float4(l[0].pos + n, 0, 1); s.Append(o);\n"
    "  o.col = l[1].col; o.pos = float4(l[1].pos - n, 0, 1); s.Append(o); o.pos = float4(l[1].pos + n, 0, 1); s.Append(o); }\n"
    "float4 ps(GO i) : SV_Target { return i.col; }\n";

static void scene_cut(const char *out) {
    ID3DBlob *vsb = NULL;
    ID3D11VertexShader *vs = mk_vs(SRC_C, "vs", &vsb);
    ID3D11GeometryShader *gs = mk_gs(SRC_C, "gs"), *gsl = mk_gs(SRC_C, "gsl");
    ID3D11PixelShader *ps = mk_ps(SRC_C, "ps");
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *lay = NULL;
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay);
    // 上半分：三角の並び（6 つ）。下半分の左：三角の帯（頂点 8 つ＝三角 6 つ）。下の右：線の並び
    struct pv v[40];
    int n = 0;
    for (int i = 0; i < 6; i++) {
        float x0 = -0.9f + (i % 3) * 0.6f, y0 = 0.1f + (i / 3) * 0.4f;
        float px[3] = { x0, x0 + 0.5f, x0 + 0.1f }, py[3] = { y0, y0 + 0.05f, y0 + 0.35f };
        for (int k = 0; k < 3; k++) {
            v[n].x = px[k]; v[n].y = py[k];
            v[n].c[0] = (uint8_t) (k == 0 ? 230 : 40 + i * 30); v[n].c[1] = (uint8_t) (k == 1 ? 220 : 60); v[n].c[2] = (uint8_t) (k == 2 ? 240 : 30 + i * 20); v[n].c[3] = 255;
            n++;
        }
    }
    int strip0 = n;
    for (int i = 0; i < 8; i++) {
        v[n].x = -0.9f + (i / 2) * 0.25f + (i % 2) * 0.05f;
        v[n].y = -0.9f + (i % 2) * 0.5f;
        v[n].c[0] = (uint8_t) (60 + i * 25); v[n].c[1] = (uint8_t) (200 - i * 20); v[n].c[2] = (uint8_t) (100 + (i % 2) * 120); v[n].c[3] = 255;
        n++;
    }
    int line0 = n;
    for (int i = 0; i < 8; i++) {
        float a = i * 0.785f;
        v[n].x = 0.5f + ((i % 2) ? 0.35f : 0.08f) * cosf(a);
        v[n].y = -0.5f + ((i % 2) ? 0.35f : 0.08f) * sinf(a);
        v[n].c[0] = 250; v[n].c[1] = (uint8_t) (i * 30); v[n].c[2] = 90; v[n].c[3] = 255;
        n++;
    }
    ID3D11Buffer *vb = mk_buf(v, sizeof(struct pv) * n, D3D11_BIND_VERTEX_BUFFER);
    target t = mk_target(1, 0);
    begin(&t, 0.05f, 0.05f, 0.05f);
    ID3D11DeviceContext_IASetInputLayout(ctx, lay);
    UINT stride = sizeof(struct pv), off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_Draw(ctx, 18, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_Draw(ctx, 8, (UINT) strip0);
    ID3D11DeviceContext_GSSetShader(ctx, gsl, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
    ID3D11DeviceContext_Draw(ctx, 8, (UINT) line0);
    finish(&t, out);
}

// ---- 層に描く GS（cube map の形）----
static const char *SRC_L =
    "cbuffer C : register(b0) { row_major float4x4 mvp[4]; };\n"
    "struct VI { float3 pos : POSITION; float2 uv : TEXCOORD; };\n"
    "struct VO { float3 pos : WPOS; float2 uv : TEXCOORD; };\n"
    "VO vs(VI i) { VO o; o.pos = i.pos; o.uv = i.uv; return o; }\n"
    "struct GO { float4 pos : SV_Position; float4 col : COLOR; uint layer : SV_RenderTargetArrayIndex; };\n"
    "[maxvertexcount(12)] void gs(triangle VO t[3], inout TriangleStream<GO> s) {\n"
    "  for (uint f = 0; f < 4; f++) {\n"
    "    for (int v = 0; v < 3; v++) { GO o; o.pos = mul(mvp[f], float4(t[v].pos, 1)); o.pos.x = o.pos.x * 0.5 - 0.5 * o.pos.w;\n"
    "      o.col = float4(t[v].uv, f / 3.0, 1); o.layer = f; s.Append(o); }\n"
    "    s.RestartStrip(); } }\n"
    "[instance(4)] [maxvertexcount(3)] void gsi(triangle VO t[3], uint f : SV_GSInstanceID, inout TriangleStream<GO> s) {\n"
    "  for (int v = 0; v < 3; v++) { GO o; o.pos = mul(mvp[f], float4(t[v].pos, 1)); o.pos.x = o.pos.x * 0.5 + 0.5 * o.pos.w;\n"
    "    o.col = float4(1 - t[v].uv.x, t[v].uv.y, 1 - f / 3.0, 1); o.layer = f; s.Append(o); } }\n"
    "float4 ps(GO i) : SV_Target { return i.col; }\n";

static void scene_layer(const char *out) {
    ID3DBlob *vsb = NULL;
    ID3D11VertexShader *vs = mk_vs(SRC_L, "vs", &vsb);
    ID3D11GeometryShader *gs = mk_gs(SRC_L, "gs"), *gsi = mk_gs(SRC_L, "gsi");
    ID3D11PixelShader *ps = mk_ps(SRC_L, "ps");
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *lay = NULL;
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay);
    struct cv v[24];
    uint16_t ix[36];
    make_cube(v, ix);
    ID3D11Buffer *vb = mk_buf(v, sizeof v, D3D11_BIND_VERTEX_BUFFER), *ib = mk_buf(ix, sizeof ix, D3D11_BIND_INDEX_BUFFER);
    float cb[64];
    for (int f = 0; f < 4; f++) make_mvp(cb + 16 * f, 0.3f + f * 0.9f, 4.0f + f * 0.6f);
    ID3D11Buffer *cbuf = mk_cb(cb, sizeof cb);
    target t = mk_target(4, 0);
    begin(&t, 0.07f, 0.04f, 0.04f);
    ID3D11DeviceContext_IASetInputLayout(ctx, lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(struct cv), off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_GSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 36, 0, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gsi, NULL, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 36, 0, 0);
    finish(&t, out);
}

// ---- 隣を持つ三角（triangleadj）：輪郭の辺を太く出す ----
static const char *SRC_A =
    "cbuffer C : register(b0) { row_major float4x4 mvp; };\n"
    "struct VI { float3 pos : POSITION; float2 uv : TEXCOORD; };\n"
    "struct VO { float4 pos : CPOS; };\n"
    "VO vs(VI i) { VO o; o.pos = mul(mvp, float4(i.pos, 1)); return o; }\n"
    "struct GO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "float facing(float4 a, float4 b, float4 c) { float2 p = a.xy / a.w, q = b.xy / b.w, r = c.xy / c.w; return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x); }\n"
    "void edge(float4 a, float4 b, float3 col, inout TriangleStream<GO> s) {\n"
    "  float2 p = a.xy / a.w, q = b.xy / b.w, d = normalize(q - p), n = float2(-d.y, d.x) * 0.02; GO o; o.col = float4(col, 1);\n"
    "  o.pos = float4(p - n, 0, 1); s.Append(o); o.pos = float4(p + n, 0, 1); s.Append(o);\n"
    "  o.pos = float4(q - n, 0, 1); s.Append(o); o.pos = float4(q + n, 0, 1); s.Append(o); s.RestartStrip(); }\n"
    "[maxvertexcount(12)] void gs(triangleadj VO t[6], inout TriangleStream<GO> s) {\n"
    "  float me = facing(t[0].pos, t[2].pos, t[4].pos); if (me <= 0) return;\n"
    "  if (facing(t[0].pos, t[1].pos, t[2].pos) <= 0) edge(t[0].pos, t[2].pos, float3(1, 0.3, 0.2), s);\n"
    "  if (facing(t[2].pos, t[3].pos, t[4].pos) <= 0) edge(t[2].pos, t[4].pos, float3(0.2, 1, 0.3), s);\n"
    "  if (facing(t[4].pos, t[5].pos, t[0].pos) <= 0) edge(t[4].pos, t[0].pos, float3(0.3, 0.4, 1), s); }\n"
    "float4 ps(GO i) : SV_Target { return i.col; }\n";

// 立方体の三角に、隣の三角の向こうの頂点を足す（同じ位置の頂点は 1 つにまとめた 8 頂点の立方体で作る）
static void scene_adj(const char *out) {
    ID3DBlob *vsb = NULL;
    ID3D11VertexShader *vs = mk_vs(SRC_A, "vs", &vsb);
    ID3D11GeometryShader *gs = mk_gs(SRC_A, "gs");
    ID3D11PixelShader *ps = mk_ps(SRC_A, "ps");
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *lay = NULL;
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay);
    struct cv v[8];
    for (int i = 0; i < 8; i++) { v[i].x = (i & 1) ? 1.f : -1.f; v[i].y = (i & 2) ? 1.f : -1.f; v[i].z = (i & 4) ? 1.f : -1.f; v[i].u = v[i].v = 0; }
    // 12 の三角（外向き）
    static const uint16_t tri[12][3] = { { 0, 2, 3 }, { 0, 3, 1 }, { 4, 5, 7 }, { 4, 7, 6 }, { 0, 4, 6 }, { 0, 6, 2 },
                                         { 1, 3, 7 }, { 1, 7, 5 }, { 2, 6, 7 }, { 2, 7, 3 }, { 0, 1, 5 }, { 0, 5, 4 } };
    uint32_t ix[72];
    for (int t = 0; t < 12; t++) {
        for (int e = 0; e < 3; e++) {
            uint16_t a = tri[t][e], b = tri[t][(e + 1) % 3], opp = a;
            for (int u = 0; u < 12; u++) {
                if (u == t) continue;
                for (int f = 0; f < 3; f++)
                    if (tri[u][f] == b && tri[u][(f + 1) % 3] == a) opp = tri[u][(f + 2) % 3];
            }
            ix[t * 6 + e * 2] = a;
            ix[t * 6 + e * 2 + 1] = opp;
        }
    }
    ID3D11Buffer *vb = mk_buf(v, sizeof v, D3D11_BIND_VERTEX_BUFFER), *ib = mk_buf(ix, sizeof ix, D3D11_BIND_INDEX_BUFFER);
    float cb[16];
    make_mvp(cb, 0.6f, 5.0f);
    ID3D11Buffer *cbuf = mk_cb(cb, sizeof cb);
    target t = mk_target(1, 0);
    begin(&t, 0.04f, 0.04f, 0.07f);
    ID3D11DeviceContext_IASetInputLayout(ctx, lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST_ADJ);
    UINT stride = sizeof(struct cv), off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R32_UINT, 0);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 72, 0, 0);
    finish(&t, out);
}

// ---- 頂点の形式の読み分け：点を受ける GS で、いろいろな形式の属性を色に混ぜる ----
static const char *SRC_F =
    "struct VI { float2 pos : POSITION; float4 c10 : CTEN; float4 bgra : CBGRA; float4 sn : SNRM; int id : SID;\n"
    "            float4 w16 : WUN; uint2 u8 : UBYTE; float2 s16 : SSHORT; };\n"
    "struct VO { float4 pos : POS; float4 col : COLOR; };\n"
    "VO vs(VI i, uint vid : SV_VertexID) { VO o;\n"
    "  o.pos = float4(i.pos - 1 + i.s16 * 0.03 + float2(i.u8) * 0.002, 0, 1);\n"
    "  float3 c = i.c10.rgb * 0.45 + i.bgra.rgb * 0.3 + (i.sn.xyz * 0.5 + 0.5) * 0.15 + i.w16.rgb * 0.1;\n"
    "  c.b += 0.03 * (i.id & 3) + 0.02 * i.c10.a + 0.01 * (vid % 5);\n"
    "  o.col = float4(c, 1); return o; }\n"
    "struct GO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "[maxvertexcount(4)] void gs(point VO p[1], inout TriangleStream<GO> s) {\n"
    "  float r = 0.03; GO o; o.col = p[0].col;\n"
    "  o.pos = p[0].pos + float4(-r, -r, 0, 0); s.Append(o); o.pos = p[0].pos + float4( r, -r, 0, 0); s.Append(o);\n"
    "  o.pos = p[0].pos + float4(-r,  r, 0, 0); s.Append(o); o.pos = p[0].pos + float4( r,  r, 0, 0); s.Append(o); }\n"
    "float4 ps(GO i) : SV_Target { return i.col; }\n";

#pragma pack(push, 1)
struct fv0 { uint16_t px, py; uint32_t c10; uint8_t bgra[4]; int8_t sn[4]; int32_t id; };
struct fv1 { uint16_t w[4]; uint8_t u[2]; uint16_t pad; int16_t s[2]; };
#pragma pack(pop)

static uint16_t f2h(float f) { // 正の数と 0 だけ（試験の値の範囲）
    uint32_t b;
    memcpy(&b, &f, 4);
    uint32_t sign = (b >> 16) & 0x8000, e = (b >> 23) & 0xff, m = b & 0x7fffff;
    if (e == 0) return (uint16_t) sign;
    int ne = (int) e - 127 + 15;
    return (uint16_t) (sign | (uint32_t) ne << 10 | (m >> 13));
}

static void scene_fmt(const char *out) {
    ID3DBlob *vsb = NULL;
    ID3D11VertexShader *vs = mk_vs(SRC_F, "vs", &vsb);
    ID3D11GeometryShader *gs = mk_gs(SRC_F, "gs");
    ID3D11PixelShader *ps = mk_ps(SRC_F, "ps");
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R16G16_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "CTEN", 0, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 4, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "CBGRA", 0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "SNRM", 0, DXGI_FORMAT_R8G8B8A8_SNORM, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "SID", 0, DXGI_FORMAT_R32_SINT, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "WUN", 0, DXGI_FORMAT_R16G16B16A16_UNORM, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "UBYTE", 0, DXGI_FORMAT_R8G8_UINT, 1, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "SSHORT", 0, DXGI_FORMAT_R16G16_SNORM, 1, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *lay = NULL;
    if (FAILED(ID3D11Device_CreateInputLayout(dev, il, 8, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay))) { printf("頂点の並びを作れない\n"); exit(1); }
    enum { N = 64 };
    struct fv0 a[N];
    struct fv1 b[N + 2];
    memset(b, 0, sizeof b);
    for (int i = 0; i < N; i++) {
        int gx = i % 8, gy = i / 8;
        float x = -0.85f + gx * 0.24f, y = -0.85f + gy * 0.24f;
        // 位置は半精度。f2h が負の数を扱わないので x+1 を持たせ、VS で 1 を引く
        a[i].px = f2h(x + 1.0f);
        a[i].py = f2h(y + 1.0f);
        uint32_t r = (uint32_t) (i * 37 % 1024), g = (uint32_t) (1023 - i * 13 % 1024), bb = (uint32_t) (i * 101 % 1024), al = (uint32_t) (i % 4);
        a[i].c10 = r | g << 10 | bb << 20 | al << 30;
        a[i].bgra[0] = (uint8_t) (i * 9); a[i].bgra[1] = (uint8_t) (200 - i * 3); a[i].bgra[2] = (uint8_t) (i * 17); a[i].bgra[3] = 255;
        a[i].sn[0] = (int8_t) (-128 + i * 4); a[i].sn[1] = (int8_t) (127 - i * 3); a[i].sn[2] = (int8_t) (i * 2 - 60); a[i].sn[3] = -128;
        a[i].id = i - 20;
        struct fv1 *q = &b[i + 2]; // 2 つ目の buffer は 2 要素ずらして、IASetVertexBuffers の offset で合わせる
        q->w[0] = (uint16_t) (i * 1000); q->w[1] = (uint16_t) (65535 - i * 700); q->w[2] = (uint16_t) (i * 333); q->w[3] = 65535;
        q->u[0] = (uint8_t) (i % 7); q->u[1] = (uint8_t) (i % 5);
        q->s[0] = (int16_t) (-32768 + i * 900); q->s[1] = (int16_t) (32767 - i * 1100);
    }
    uint16_t ix[60];
    for (int i = 0; i < 60; i++) ix[i] = (uint16_t) (4 + (i * 11) % 60); // BaseVertex -4 で 0〜59 を指す
    ID3D11Buffer *vb0 = mk_buf(a, sizeof a, D3D11_BIND_VERTEX_BUFFER), *vb1 = mk_buf(b, sizeof b, D3D11_BIND_VERTEX_BUFFER),
                 *ib = mk_buf(ix, sizeof ix, D3D11_BIND_INDEX_BUFFER);
    target t = mk_target(1, 0);
    begin(&t, 0.03f, 0.03f, 0.03f);
    ID3D11DeviceContext_IASetInputLayout(ctx, lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    UINT strides[2] = { sizeof(struct fv0), sizeof(struct fv1) }, offs[2] = { 0, 2 * sizeof(struct fv1) };
    ID3D11Buffer *vbs[2] = { vb0, vb1 };
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 2, vbs, strides, offs);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_Draw(ctx, 6, 58);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 40, 3, -4);
    finish(&t, out);
}

// ---- 番号つきの三角の帯（D3D11 には扇が無い）。2 つ目の描画は途中に restart の値（0xffff）を入れる ----
static void scene_strip(const char *out) {
    ID3DBlob *vsb = NULL;
    ID3D11VertexShader *vs = mk_vs(SRC_C, "vs", &vsb);
    ID3D11GeometryShader *gs = mk_gs(SRC_C, "gs");
    ID3D11PixelShader *ps = mk_ps(SRC_C, "ps");
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *lay = NULL;
    ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay);
    struct pv v[20];
    for (int i = 0; i < 20; i++) {
        v[i].x = -0.9f + (i / 2) * 0.19f + (i % 2) * 0.04f;
        v[i].y = (i % 2) ? 0.6f : 0.1f;
        v[i].c[0] = (uint8_t) (40 + i * 10); v[i].c[1] = (uint8_t) (230 - i * 9); v[i].c[2] = (uint8_t) ((i % 3) * 100); v[i].c[3] = 255;
    }
    // 上：restart の無い帯。下：6 番に restart の値がある帯（後ろの帯は 7 番＝奇数の位置から始まる）
    uint16_t ix[32] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
    uint16_t ix2[16] = { 0, 1, 2, 3, 4, 5, 0xffff, 8, 9, 10, 11, 12, 13 };
    ID3D11Buffer *vb = mk_buf(v, sizeof v, D3D11_BIND_VERTEX_BUFFER), *ib = mk_buf(ix, sizeof ix, D3D11_BIND_INDEX_BUFFER),
                 *ib2 = mk_buf(ix2, sizeof ix2, D3D11_BIND_INDEX_BUFFER);
    target t = mk_target(1, 0);
    begin(&t, 0.05f, 0.05f, 0.05f);
    ID3D11DeviceContext_IASetInputLayout(ctx, lay);
    UINT stride = sizeof(struct pv), off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 10, 1, 0);
    // 下半分へずらすため、同じ頂点を y だけ変えた 2 つ目の buffer
    for (int i = 0; i < 20; i++) v[i].y -= 0.9f;
    ID3D11Buffer *vb2 = mk_buf(v, sizeof v, D3D11_BIND_VERTEX_BUFFER);
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb2, &stride, &off);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib2, DXGI_FORMAT_R16_UINT, 0);
    ID3D11DeviceContext_DrawIndexed(ctx, 13, 0, 0);
    finish(&t, out);
}

// ---- 重さを測る：格子の網（三角 32768 個）か、点 262144 個を、決めた回数描いて、1 コマの時間を出す ----
//   perf_none   GS なし（VS で変換）。ドライバ越しの往復などの分を見る対照
//   perf_tri    三角を受けて 3 つ出す GS（帯のまま）
//   perf_layer  三角を受けて 4 つの層に 3 つずつ出す GS（ループと RestartStrip・並びに直る）
//   perf_ginst  [instance(4)] で 4 つの層に出す GS
//   perf_pts    点を四角に広げる GS（番号なし＝B）。perf_ptsidx は番号つき（C1）
static const char *SRC_N =
    "cbuffer C : register(b0) { row_major float4x4 mvp[4]; };\n"
    "struct VI { float3 pos : POSITION; float2 uv : TEXCOORD; };\n"
    "struct VO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VO vs(VI i) { VO o; o.pos = mul(mvp[0], float4(i.pos, 1)); o.col = float4(i.uv, 0.5, 1); return o; }\n"
    "float4 ps(VO i) : SV_Target { return i.col; }\n";
static const char *SRC_PP =
    "struct VI { float2 pos : POSITION; float4 col : COLOR; };\n"
    "struct VO { float4 pos : POS; float4 col : COLOR; };\n"
    "VO vs(VI i) { VO o; o.pos = float4(i.pos, 0, 1); o.col = i.col; return o; }\n"
    "struct GO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "[maxvertexcount(4)] void gs(point VO p[1], inout TriangleStream<GO> s) {\n"
    "  float r = 0.004; GO o; o.col = p[0].col;\n"
    "  o.pos = p[0].pos + float4(-r, -r, 0, 0); s.Append(o); o.pos = p[0].pos + float4( r, -r, 0, 0); s.Append(o);\n"
    "  o.pos = p[0].pos + float4(-r,  r, 0, 0); s.Append(o); o.pos = p[0].pos + float4( r,  r, 0, 0); s.Append(o); }\n"
    "float4 ps(GO i) : SV_Target { return i.col; }\n";

static int g_reps = 4; // 1 コマに描く回数（GSTEST_REPS）
static void scene_perf(const char *kind, const char *out, int frames) {
    int pts = !strncmp(kind, "perf_pts", 8), idx = !strcmp(kind, "perf_ptsidx"), layered = !strcmp(kind, "perf_layer") || !strcmp(kind, "perf_ginst");
    const char *src = pts ? SRC_PP : !strcmp(kind, "perf_none") ? SRC_N : !strcmp(kind, "perf_tri") ? SRC_T : SRC_L;
    const char *gsn = !strcmp(kind, "perf_ginst") ? "gsi" : "gs";
    ID3DBlob *vsb = NULL;
    ID3D11VertexShader *vs = mk_vs(src, "vs", &vsb);
    ID3D11GeometryShader *gs = strcmp(kind, "perf_none") ? mk_gs(src, gsn) : NULL;
    ID3D11PixelShader *ps = mk_ps(src, "ps");
    ID3D11InputLayout *lay = NULL;
    ID3D11Buffer *vb = NULL, *ib = NULL;
    UINT stride, off = 0, count;
    if (pts) {
        D3D11_INPUT_ELEMENT_DESC il[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay);
        enum { NP = 262144 };
        struct pv *v = malloc(sizeof(struct pv) * NP);
        uint32_t *ix = malloc(4 * NP);
        for (int i = 0; i < NP; i++) {
            v[i].x = -0.98f + (i % 512) * (1.96f / 512); v[i].y = -0.98f + (i / 512) * (1.96f / 512);
            v[i].c[0] = (uint8_t) (i * 7); v[i].c[1] = (uint8_t) (i >> 9); v[i].c[2] = 128; v[i].c[3] = 255;
            ix[i] = (uint32_t) ((i * 7919u) % NP);
        }
        vb = mk_buf(v, sizeof(struct pv) * NP, D3D11_BIND_VERTEX_BUFFER);
        ib = mk_buf(ix, 4 * NP, D3D11_BIND_INDEX_BUFFER);
        stride = sizeof(struct pv);
        count = NP;
    } else {
        D3D11_INPUT_ELEMENT_DESC il[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay);
        enum { G = 128 };
        struct cv *v = malloc(sizeof(struct cv) * (G + 1) * (G + 1));
        uint16_t *ix = malloc(2 * G * G * 6);
        for (int y = 0; y <= G; y++)
            for (int x = 0; x <= G; x++) {
                struct cv *p = &v[y * (G + 1) + x];
                p->x = -1.5f + 3.0f * x / G; p->y = -1.5f + 3.0f * y / G; p->z = 0.2f * sinf(x * 0.2f) * cosf(y * 0.15f);
                p->u = (float) x / G; p->v = (float) y / G;
            }
        int n = 0;
        for (int y = 0; y < G; y++)
            for (int x = 0; x < G; x++) {
                uint16_t a = (uint16_t) (y * (G + 1) + x), b = (uint16_t) (a + 1), c = (uint16_t) (a + G + 1), d = (uint16_t) (c + 1);
                ix[n++] = a; ix[n++] = b; ix[n++] = d; ix[n++] = a; ix[n++] = d; ix[n++] = c;
            }
        vb = mk_buf(v, sizeof(struct cv) * (G + 1) * (G + 1), D3D11_BIND_VERTEX_BUFFER);
        ib = mk_buf(ix, 2 * (UINT) n, D3D11_BIND_INDEX_BUFFER);
        stride = sizeof(struct cv);
        count = (UINT) n;
    }
    float cb[64];
    for (int f = 0; f < 4; f++) make_mvp(cb + 16 * f, 0.3f + f * 0.9f, 4.0f + f * 0.6f);
    float cbt[20];
    make_mvp(cbt, 0.6f, 5.0f);
    cbt[16] = 0.0f; cbt[17] = cbt[18] = cbt[19] = 0;
    ID3D11Buffer *cbuf = !strcmp(kind, "perf_tri") ? mk_cb(cbt, sizeof cbt) : mk_cb(cb, sizeof cb);
    target t = mk_target(layered ? 4 : 1, 0);
    // コマの終わりは、1 画素を staging に写して Map して待つ（EVENT のクエリはドライバ越しで返ってこなかった）
    D3D11_TEXTURE2D_DESC sd1 = { .Width = 1, .Height = 1, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                 .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_STAGING, .CPUAccessFlags = D3D11_CPU_ACCESS_READ };
    ID3D11Texture2D *one = NULL;
    ID3D11Device_CreateTexture2D(dev, &sd1, NULL, &one);
    D3D11_BOX box = { 128, 128, 0, 129, 129, 1 };
    LARGE_INTEGER fq, t0 = { 0 }, t1;
    QueryPerformanceFrequency(&fq);
    int warm = 5;
    for (int f = 0; f < warm + frames; f++) {
        if (f == warm) QueryPerformanceCounter(&t0);
        begin(&t, 0.05f, 0.05f, 0.05f);
        ID3D11DeviceContext_IASetInputLayout(ctx, lay);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, pts ? D3D11_PRIMITIVE_TOPOLOGY_POINTLIST : D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off);
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, pts ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT, 0);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cbuf);
        ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
        ID3D11DeviceContext_GSSetConstantBuffers(ctx, 0, 1, &cbuf);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        for (int r = 0; r < g_reps; r++) {
            if (pts && !idx) ID3D11DeviceContext_Draw(ctx, count, 0);
            else ID3D11DeviceContext_DrawIndexed(ctx, count, 0, 0);
        }
        D3D11_MAPPED_SUBRESOURCE mm;
        ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *) one, 0, 0, 0, 0, (ID3D11Resource *) t.tex, 0, &box);
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) one, 0, D3D11_MAP_READ, 0, &mm))) ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) one, 0);
    }
    QueryPerformanceCounter(&t1);
    printf("%s：1 コマ %.2f ms（%d コマの平均・1 コマに %u 個 x %d 回）\n", kind, 1000.0 * (t1.QuadPart - t0.QuadPart) / fq.QuadPart / frames, frames, count, g_reps);
    finish(&t, out);
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 3) { printf("gstest <pidx|pinst|tri|cut|layer|adj> <out.ppm>\n"); return 2; }
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    g_compile = dc ? (PFN_COMPILE) GetProcAddress(dc, "D3DCompile") : NULL;
    if (!g_compile) { printf("d3dcompiler_47.dll が無い\n"); return 1; }
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0 }, got;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 1, D3D11_SDK_VERSION, &dev, &got, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDevice が失敗（0x%08lx）\n", hr); return 1; }
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC desc = { 0 };
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **) &xd)) && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)))
        IDXGIAdapter_GetDesc(ad, &desc);
    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, NULL, NULL);
    printf("装置: %s\n", name);
    const char *s = argv[1];
    if (!strcmp(s, "pidx")) scene_points(0, argv[2]);
    else if (!strcmp(s, "pinst")) scene_points(1, argv[2]);
    else if (!strcmp(s, "tri")) scene_tri(argv[2]);
    else if (!strcmp(s, "cut")) scene_cut(argv[2]);
    else if (!strcmp(s, "layer")) scene_layer(argv[2]);
    else if (!strcmp(s, "adj")) scene_adj(argv[2]);
    else if (!strcmp(s, "fmt")) scene_fmt(argv[2]);
    else if (!strcmp(s, "strip")) scene_strip(argv[2]);
    else if (!strncmp(s, "perf_", 5)) {
        const char *r = getenv("GSTEST_REPS");
        if (r && atoi(r) > 0) g_reps = atoi(r);
        scene_perf(s, argv[2], argc > 3 ? atoi(argv[3]) : 30);
    }
    else { printf("知らない場面 %s\n", s); return 2; }
    ID3D11DeviceContext_Flush(ctx);
    return 0;
}
