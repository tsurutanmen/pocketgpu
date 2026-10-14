// DirectX 9 で、三角形を 1 枚描いて、絵を PC に持ち帰る試験（Need for Speed Carbon のような古いゲームの代わり）。
// 横に DXVK の d3d9.dll を置くと、DirectX 9 の頼みが Vulkan に直される。
// 窓は作るが、見せない（ShowWindow しない）。描く先は窓ではなく、別に作った表面。
//   d3d9tri.exe [出す PPM のファイル名]
// アダプターの名前（DXVK なら Vulkan の GPU の名前）と、色のついた画素の数・全部の画素から作った検算の値を出す
#include <windows.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>

enum { W = 256, H = 256 };

typedef struct { float x, y, z, rhw; DWORD color; } Vtx;

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    const char *out = argc > 1 ? argv[1] : "d3d9tri.ppm";
    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "d3d9tri";
    RegisterClassA(&wc);
    // 主のモニターに出さない：サブ（左）の座標に置く。見せない窓なので何も映らない
    HWND wnd = CreateWindowA("d3d9tri", "d3d9tri", WS_POPUP, -1800, 100, W, H, NULL, NULL, wc.hInstance, NULL);
    if (!wnd) { printf("窓を作れない\n"); return 1; }

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { printf("Direct3DCreate9 が失敗\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id;
    if (SUCCEEDED(IDirect3D9_GetAdapterIdentifier(d3d, 0, 0, &id)))
        printf("アダプター: %s（ドライバ %s・vendor 0x%04lx）\n", id.Description, id.Driver, (unsigned long) id.VendorId);

    D3DPRESENT_PARAMETERS pp = { 0 };
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = W;
    pp.BackBufferHeight = H;
    pp.hDeviceWindow = wnd;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { printf("CreateDevice が失敗（0x%08lx）\n", (unsigned long) hr); return 1; }

    IDirect3DSurface9 *rt = NULL, *sys = NULL;
    if (FAILED(IDirect3DDevice9_CreateRenderTarget(dev, W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL))) { printf("描く先を作れない\n"); return 1; }
    if (FAILED(IDirect3DDevice9_CreateOffscreenPlainSurface(dev, W, H, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, NULL))) { printf("読む先を作れない\n"); return 1; }
    IDirect3DDevice9_SetRenderTarget(dev, 0, rt);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_CULLMODE, D3DCULL_NONE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_LIGHTING, FALSE);
    IDirect3DDevice9_SetRenderState(dev, D3DRS_ZENABLE, FALSE);

    Vtx v[3] = {
        { 128.0f, 40.0f, 0.5f, 1.0f, 0xffff0000 },
        { 216.0f, 216.0f, 0.5f, 1.0f, 0xff00ff00 },
        { 40.0f, 216.0f, 0.5f, 1.0f, 0xff0000ff },
    };
    double t0 = (double) GetTickCount64();
    IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, 0xff1a1a1a, 1.0f, 0);
    IDirect3DDevice9_BeginScene(dev);
    IDirect3DDevice9_SetFVF(dev, D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
    hr = IDirect3DDevice9_DrawPrimitiveUP(dev, D3DPT_TRIANGLELIST, 1, v, sizeof(Vtx));
    if (FAILED(hr)) { printf("DrawPrimitiveUP が失敗（0x%08lx）\n", (unsigned long) hr); return 1; }
    IDirect3DDevice9_EndScene(dev);
    if (FAILED(IDirect3DDevice9_GetRenderTargetData(dev, rt, sys))) { printf("絵を読み出せない\n"); return 1; }
    D3DLOCKED_RECT lr;
    if (FAILED(IDirect3DSurface9_LockRect(sys, &lr, NULL, D3DLOCK_READONLY))) { printf("絵を読めない\n"); return 1; }
    double t1 = (double) GetTickCount64();

    uint32_t colored = 0, sum = 2166136261u;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *p = (const uint8_t *) lr.pBits + (size_t) y * lr.Pitch + (size_t) x * 4; // B G R A の順
            if (p[0] > 40 || p[1] > 40 || p[2] > 40) colored++;
            for (int k = 0; k < 4; k++) sum = (sum ^ p[k]) * 16777619u;
        }
    printf("描いた（%d x %d・%.0f ms）：色のついた画素 %u 個・検算 %08x\n", W, H, t1 - t0, colored, sum);
    FILE *f = fopen(out, "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const uint8_t *p = (const uint8_t *) lr.pBits + (size_t) y * lr.Pitch + (size_t) x * 4;
                uint8_t rgb[3] = { p[2], p[1], p[0] };
                fwrite(rgb, 1, 3, f);
            }
        fclose(f);
        printf("絵を %s に書いた\n", out);
    }
    IDirect3DSurface9_UnlockRect(sys);
    IDirect3DSurface9_Release(sys);
    IDirect3DSurface9_Release(rt);
    IDirect3DDevice9_Release(dev);
    IDirect3D9_Release(d3d);
    return colored > 1000 ? 0 : 1;
}
