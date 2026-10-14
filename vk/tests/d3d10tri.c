// DirectX 10 で、窓を使わずに三角形を 1 枚描いて、絵を PC に持ち帰る試験。
// 横に DXVK の d3d10core.dll・d3d11.dll・dxgi.dll を置くと、DirectX 10 の頼みが Vulkan に直される
// （Windows の d3d10.dll が、横の d3d10core.dll を読む。DXVK 1.10 に d3d10.dll は無い）。
//   d3d10tri.exe [出す PPM のファイル名]
// 描いた絵の「色のついた画素の数」と「全部の画素から作った検算の値」を出す
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d10.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum { W = 256, H = 256 };

static const char *SRC =
    "struct V { float4 pos : SV_Position; float3 col : COLOR; };\n"
    "V vs(uint id : SV_VertexID) {\n"
    "  float2 p[3] = { float2(0.0, -0.6), float2(0.6, 0.6), float2(-0.6, 0.6) };\n"
    "  float3 c[3] = { float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1) };\n"
    "  V o; o.pos = float4(p[id].x, -p[id].y, 0, 1); o.col = c[id]; return o;\n"
    "}\n"
    "float4 ps(V i) : SV_Target { return float4(i.col, 1); }\n";

typedef HRESULT (WINAPI *PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static ID3DBlob *compile(PFN_COMPILE fn, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = fn(SRC, strlen(SRC), "tri.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) {
        printf("シェーダーを組めない（%s）：%s\n", entry, err ? (const char *) ID3D10Blob_GetBufferPointer(err) : "?");
        return NULL;
    }
    return code;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const char *out = argc > 1 ? argv[1] : "d3d10tri.ppm";
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    PFN_COMPILE D3DCompileFn = dc ? (PFN_COMPILE) GetProcAddress(dc, "D3DCompile") : NULL;
    if (!D3DCompileFn) { printf("d3dcompiler_47.dll が無い\n"); return 1; }

    ID3D10Device *dev = NULL;
    HRESULT hr = D3D10CreateDevice(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &dev);
    if (FAILED(hr)) { printf("D3D10CreateDevice が失敗（0x%08lx）\n", (unsigned long) hr); return 1; }
    // どの GPU で動いているか
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC desc = { 0 };
    if (SUCCEEDED(ID3D10Device_QueryInterface(dev, &IID_IDXGIDevice, (void **) &xd)) && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad)))
        IDXGIAdapter_GetDesc(ad, &desc);
    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, NULL, NULL);
    printf("装置: %s\n", name);

    D3D10_TEXTURE2D_DESC td = { .Width = W, .Height = H, .MipLevels = 1, .ArraySize = 1, .Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .SampleDesc = { 1, 0 }, .Usage = D3D10_USAGE_DEFAULT, .BindFlags = D3D10_BIND_RENDER_TARGET };
    ID3D10Texture2D *rt = NULL, *st = NULL;
    ID3D10RenderTargetView *rtv = NULL;
    if (FAILED(ID3D10Device_CreateTexture2D(dev, &td, NULL, &rt))) { printf("描く先を作れない\n"); return 1; }
    ID3D10Device_CreateRenderTargetView(dev, (ID3D10Resource *) rt, NULL, &rtv);
    td.Usage = D3D10_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    ID3D10Device_CreateTexture2D(dev, &td, NULL, &st);

    ID3DBlob *vsb = compile(D3DCompileFn, "vs", "vs_4_0"), *psb = compile(D3DCompileFn, "ps", "ps_4_0");
    if (!vsb || !psb) return 1;
    ID3D10VertexShader *vs = NULL;
    ID3D10PixelShader *ps = NULL;
    ID3D10Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &vs);
    ID3D10Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), &ps);

    double t0 = (double) GetTickCount64();
    float clear[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    ID3D10Device_ClearRenderTargetView(dev, rtv, clear);
    ID3D10Device_OMSetRenderTargets(dev, 1, &rtv, NULL);
    D3D10_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    ID3D10Device_RSSetViewports(dev, 1, &vp);
    ID3D10Device_IASetPrimitiveTopology(dev, D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D10Device_VSSetShader(dev, vs);
    ID3D10Device_PSSetShader(dev, ps);
    ID3D10Device_Draw(dev, 3, 0);
    ID3D10Device_CopyResource(dev, (ID3D10Resource *) st, (ID3D10Resource *) rt);
    D3D10_MAPPED_TEXTURE2D m;
    if (FAILED(ID3D10Texture2D_Map(st, 0, D3D10_MAP_READ, 0, &m))) { printf("絵を読めない\n"); return 1; }
    double t1 = (double) GetTickCount64();

    uint32_t colored = 0, sum = 2166136261u;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *p = (const uint8_t *) m.pData + (size_t) y * m.RowPitch + (size_t) x * 4;
            if (p[0] > 40 || p[1] > 40 || p[2] > 40) colored++;
            for (int k = 0; k < 4; k++) sum = (sum ^ p[k]) * 16777619u;
        }
    printf("描いた（%d x %d・%.0f ms）：色のついた画素 %u 個・検算 %08x\n", W, H, t1 - t0, colored, sum);
    FILE *f = fopen(out, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) fwrite((const uint8_t *) m.pData + (size_t) y * m.RowPitch + (size_t) x * 4, 1, 3, f);
        fclose(f);
        printf("絵を %s に書いた\n", out);
    }
    ID3D10Texture2D_Unmap(st, 0);
    return colored > 1000 ? 0 : 1;
}
