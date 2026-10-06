// DirectX 11 で、窓に回る立方体を出す試験。ゲームでよく使う物をひと通り使う：
// 窓と swapchain・頂点と index の buffer・毎コマ書き換える定数の buffer・テクスチャと sampler・奥行きの buffer。
// 横に DXVK の d3d11.dll・dxgi.dll を置くと、DirectX の頼みが Vulkan に直される。
//   d3d11cube.exe [コマ数] [出す PPM のファイル名] [bc1]
// bc1 を付けると、テクスチャを BC1（DirectX のゲームがよく使う圧縮）で持つ
// 決めたコマ数を出して 1 秒あたりのコマ数を出す。最後に、決めた角度の 1 枚を窓の外に描いて、検算の値を出す
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

enum { W = 640, H = 480, TEX = 64 };

static const char *SRC =
    "cbuffer C : register(b0) { row_major float4x4 mvp; };\n"
    "struct VI { float3 pos : POSITION; float2 uv : TEXCOORD; float shade : SHADE; };\n"
    "struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD; float shade : SHADE; };\n"
    "VO vs(VI i) { VO o; o.pos = mul(mvp, float4(i.pos, 1)); o.uv = i.uv; o.shade = i.shade; return o; }\n"
    "Texture2D tex : register(t0);\n"
    "SamplerState smp : register(s0);\n"
    "float4 ps(VO i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb * i.shade, 1); }\n";

typedef HRESULT (WINAPI *PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static ID3DBlob *compile(PFN_COMPILE fn, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = fn(SRC, strlen(SRC), "cube.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        printf("シェーダーを組めない（%s）：%s\n", entry, err ? (const char *) ID3D10Blob_GetBufferPointer(err) : "?");
        return NULL;
    }
    return code;
}

struct vtx { float x, y, z, u, v, shade; };

// 面ごとに 4 つの頂点。面ごとに明るさを変えて、向きが分かるようにする
static void make_cube(struct vtx *v, uint16_t *ix) {
    static const float n[6][3] = { { 0, 0, -1 }, { 0, 0, 1 }, { -1, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    static const float shade[6] = { 1.0f, 0.55f, 0.8f, 0.7f, 0.9f, 0.6f };
    for (int f = 0; f < 6; f++) {
        float nx = n[f][0], ny = n[f][1], nz = n[f][2];
        // 面の上で直交する 2 つの向き
        float ax = ny != 0 ? 1 : 0, ay = 0, az = ny != 0 ? 0 : (nx != 0 ? 1 : 0);
        if (nz != 0) { ax = 1; az = 0; }
        float bx = ny * az - nz * ay, by = nz * ax - nx * az, bz = nx * ay - ny * ax;
        static const float c[4][2] = { { -1, -1 }, { 1, -1 }, { 1, 1 }, { -1, 1 } };
        for (int k = 0; k < 4; k++) {
            struct vtx *p = &v[f * 4 + k];
            p->x = nx + c[k][0] * ax + c[k][1] * bx;
            p->y = ny + c[k][0] * ay + c[k][1] * by;
            p->z = nz + c[k][0] * az + c[k][1] * bz;
            p->u = (c[k][0] + 1) * 0.5f;
            p->v = (c[k][1] + 1) * 0.5f;
            p->shade = shade[f];
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

// 角度 t の立方体を、少し離れたところから見る（左手系・奥行きは 0〜1）
static void make_mvp(float *m, float t) {
    float cy = cosf(t), sy = sinf(t), cx = cosf(t * 0.7f), sx = sinf(t * 0.7f);
    float ry[16] = { cy, 0, sy, 0, 0, 1, 0, 0, -sy, 0, cy, 0, 0, 0, 0, 1 };
    float rx[16] = { 1, 0, 0, 0, 0, cx, -sx, 0, 0, sx, cx, 0, 0, 0, 0, 1 };
    float tr[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 4.0f, 0, 0, 0, 1 };
    float zn = 0.1f, zf = 100.0f, f = 1.0f / tanf(0.5f * 1.0f), a = (float) W / H;
    float pr[16] = { f / a, 0, 0, 0, 0, f, 0, 0, 0, 0, zf / (zf - zn), -zn * zf / (zf - zn), 0, 0, 1, 0 };
    float r[16];
    mat_mul(r, ry, rx);
    mat_mul(r, tr, r);
    mat_mul(m, pr, r);
}

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, msg, wp, lp);
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    int frames = argc > 1 ? atoi(argv[1]) : 300;
    const char *out = argc > 2 ? argv[2] : "d3d11cube.ppm";
    int bc1 = argc > 3 && !strcmp(argv[3], "bc1");
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_COMPILE D3DCompileFn = dc ? (PFN_COMPILE) GetProcAddress(dc, "D3DCompile") : NULL;
    if (!D3DCompileFn) { printf("d3dcompiler_47.dll が無い\n"); return 1; }

    WNDCLASSW wc = { .lpfnWndProc = wndproc, .hInstance = GetModuleHandleW(NULL), .lpszClassName = L"d3d11cube", .hCursor = LoadCursor(NULL, IDC_ARROW) };
    RegisterClassW(&wc);
    RECT rc = { 0, 0, W, H };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"d3d11cube", L"d3d11cube", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                              rc.right - rc.left, rc.bottom - rc.top, NULL, NULL, wc.hInstance, NULL);

    DXGI_SWAP_CHAIN_DESC sd = { .BufferDesc = { .Width = W, .Height = H, .Format = DXGI_FORMAT_R8G8B8A8_UNORM }, .SampleDesc = { 1, 0 },
                                .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, .BufferCount = 2, .OutputWindow = hwnd, .Windowed = TRUE,
                                .SwapEffect = DXGI_SWAP_EFFECT_DISCARD };
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }, got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 3, D3D11_SDK_VERSION, &sd, &sc, &dev, &got, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDeviceAndSwapChain が失敗（0x%08lx）\n", hr); return 1; }
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC desc = { 0 };
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **) &xd)) && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)))
        IDXGIAdapter_GetDesc(ad, &desc);
    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, NULL, NULL);
    printf("装置: %s（feature level %x）\n", name, (unsigned) got);

    // 窓の絵（back buffer）と、決めた角度の 1 枚を描く先
    ID3D11Texture2D *bb = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **) &bb);
    ID3D11RenderTargetView *bbv = NULL;
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *) bb, NULL, &bbv);
    D3D11_TEXTURE2D_DESC td = { .Width = W, .Height = H, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_DEFAULT, .BindFlags = D3D11_BIND_RENDER_TARGET };
    ID3D11Texture2D *rt = NULL, *st = NULL, *ds = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11DepthStencilView *dsv = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &rt);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *) rt, NULL, &rtv);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &st);
    td.Usage = D3D11_USAGE_DEFAULT;
    td.CPUAccessFlags = 0;
    td.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &td, NULL, &ds))) { printf("奥行きの buffer を作れない\n"); return 1; }
    ID3D11Device_CreateDepthStencilView(dev, (ID3D11Resource *) ds, NULL, &dsv);

    // 市松模様のテクスチャ
    uint32_t *px = malloc(TEX * TEX * 4);
    for (int y = 0; y < TEX; y++)
        for (int x = 0; x < TEX; x++) px[y * TEX + x] = ((x / 8 + y / 8) & 1) ? 0xff2080f0u : 0xfff0e0c0u;
    D3D11_TEXTURE2D_DESC xt = { .Width = TEX, .Height = TEX, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
                                .SampleDesc = { 1, 0 }, .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_SHADER_RESOURCE };
    D3D11_SUBRESOURCE_DATA xd0 = { px, TEX * 4, 0 };
    // BC1：4x4 の塊ごとに 2 色と 2 ビットの番号。市松の 1 マスは塊 2 つ分なので、塊の中は 1 色になる
    uint8_t *bc = malloc((TEX / 4) * (TEX / 4) * 8);
    if (bc1) {
        for (int by = 0; by < TEX / 4; by++)
            for (int bx = 0; bx < TEX / 4; bx++) {
                uint32_t c = px[by * 4 * TEX + bx * 4];
                uint16_t c565 = (uint16_t) ((((c >> 3) & 0x1f) << 11) | (((c >> 10) & 0x3f) << 5) | ((c >> 19) & 0x1f));
                uint8_t *b = bc + (by * (TEX / 4) + bx) * 8;
                b[0] = b[2] = (uint8_t) c565;
                b[1] = b[3] = (uint8_t) (c565 >> 8);
                b[4] = b[5] = b[6] = b[7] = 0;
            }
        xt.Format = DXGI_FORMAT_BC1_UNORM;
        xd0.pSysMem = bc;
        xd0.SysMemPitch = (TEX / 4) * 8;
        printf("テクスチャは BC1\n");
    }
    ID3D11Texture2D *tex = NULL;
    ID3D11ShaderResourceView *srv = NULL;
    if (FAILED(ID3D11Device_CreateTexture2D(dev, &xt, &xd0, &tex))) { printf("テクスチャを作れない\n"); return 1; }
    ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *) tex, NULL, &srv);
    D3D11_SAMPLER_DESC smd = { .Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR, .AddressU = D3D11_TEXTURE_ADDRESS_WRAP, .AddressV = D3D11_TEXTURE_ADDRESS_WRAP,
                               .AddressW = D3D11_TEXTURE_ADDRESS_WRAP, .ComparisonFunc = D3D11_COMPARISON_NEVER, .MaxLOD = D3D11_FLOAT32_MAX };
    ID3D11SamplerState *smp = NULL;
    ID3D11Device_CreateSamplerState(dev, &smd, &smp);

    struct vtx v[24];
    uint16_t ix[36];
    make_cube(v, ix);
    D3D11_BUFFER_DESC bd = { .ByteWidth = sizeof v, .Usage = D3D11_USAGE_IMMUTABLE, .BindFlags = D3D11_BIND_VERTEX_BUFFER };
    D3D11_SUBRESOURCE_DATA bd0 = { v, 0, 0 };
    ID3D11Buffer *vb = NULL, *ib = NULL, *cb = NULL;
    ID3D11Device_CreateBuffer(dev, &bd, &bd0, &vb);
    bd.ByteWidth = sizeof ix;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    bd0.pSysMem = ix;
    ID3D11Device_CreateBuffer(dev, &bd, &bd0, &ib);
    D3D11_BUFFER_DESC cbd = { .ByteWidth = 64, .Usage = D3D11_USAGE_DYNAMIC, .BindFlags = D3D11_BIND_CONSTANT_BUFFER, .CPUAccessFlags = D3D11_CPU_ACCESS_WRITE };
    ID3D11Device_CreateBuffer(dev, &cbd, NULL, &cb);

    ID3DBlob *vsb = compile(D3DCompileFn, "vs", "vs_4_0"), *psb = compile(D3DCompileFn, "ps", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D11VertexShader *vs = NULL;
    ID3D11PixelShader *ps = NULL;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "SHADE", 0, DXGI_FORMAT_R32_FLOAT, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *lay = NULL;
    if (FAILED(ID3D11Device_CreateInputLayout(dev, il, 3, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &lay))) {
        printf("頂点の並びを作れない\n");
        return 1;
    }
    D3D11_RASTERIZER_DESC rd = { .FillMode = D3D11_FILL_SOLID, .CullMode = D3D11_CULL_NONE, .DepthClipEnable = TRUE };
    ID3D11RasterizerState *rs = NULL;
    ID3D11Device_CreateRasterizerState(dev, &rd, &rs);

    UINT stride = sizeof(struct vtx), off = 0;
    D3D11_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    float clear[4] = { 0.08f, 0.09f, 0.11f, 1.0f };

    // 1 コマ描く（rtv に、角度 t で）
    #define DRAW(view, t) do { \
        D3D11_MAPPED_SUBRESOURCE mm; \
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mm))) { \
            make_mvp((float *) mm.pData, (t)); \
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) cb, 0); \
        } \
        ID3D11DeviceContext_ClearRenderTargetView(ctx, (view), clear); \
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0); \
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &(view), dsv); \
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp); \
        ID3D11DeviceContext_RSSetState(ctx, rs); \
        ID3D11DeviceContext_IASetInputLayout(ctx, lay); \
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); \
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &off); \
        ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0); \
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0); \
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb); \
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0); \
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv); \
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &smp); \
        ID3D11DeviceContext_DrawIndexed(ctx, 36, 0, 0); \
    } while (0)

    LARGE_INTEGER fq, t0, t1;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    int shown = 0;
    for (; shown < frames; shown++) {
        MSG msg;
        int quit = 0;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) quit = 1;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        DRAW(bbv, shown * 0.03f);
        if (FAILED(IDXGISwapChain_Present(sc, 0, 0))) { printf("Present が失敗\n"); break; }
    }
    QueryPerformanceCounter(&t1);
    double sec = (double) (t1.QuadPart - t0.QuadPart) / fq.QuadPart;
    printf("窓に %d コマ・%.2f 秒・%.1f コマ/秒\n", shown, sec, shown / sec);

    // 決めた角度の 1 枚を窓の外に描いて読む
    DRAW(rtv, 0.6f);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *) st, (ID3D11Resource *) rt);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) st, 0, D3D11_MAP_READ, 0, &m))) { printf("絵を読めない\n"); return 1; }
    uint32_t colored = 0, sum = 2166136261u;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *p = (const uint8_t *) m.pData + (size_t) y * m.RowPitch + (size_t) x * 4;
            if (p[0] > 60 || p[1] > 60 || p[2] > 60) colored++;
            for (int k = 0; k < 4; k++) sum = (sum ^ p[k]) * 16777619u;
        }
    printf("決めた角度の絵（%d x %d）：色のついた画素 %u 個・検算 %08x\n", W, H, colored, sum);
    FILE *f = fopen(out, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) fwrite((const uint8_t *) m.pData + (size_t) y * m.RowPitch + (size_t) x * 4, 1, 3, f);
        fclose(f);
        printf("絵を %s に書いた\n", out);
    }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) st, 0);
    DestroyWindow(hwnd);
    return colored > 10000 ? 0 : 1;
}
