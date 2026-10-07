// DirectX 11 の EVENT のクエリが「終わった」を返すかを確かめる。ゲームはコマの先走りを抑えるのによく使う。
//   d3d11event.exe [回数]
// 毎回、画面の外の的を塗りつぶして EVENT のクエリを置き、GetData が S_OK を返すまで待つ。5 秒で諦める
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>

static double now_ms(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double) c.QuadPart * 1000.0 / (double) f.QuadPart;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    int n = argc > 1 ? atoi(argv[1]) : 20;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    if (FAILED(hr)) { printf("装置を作れない 0x%08lx\n", hr); return 1; }
    D3D11_TEXTURE2D_DESC td = { 256, 256, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0 };
    ID3D11Texture2D *tex = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    ID3D11Device_CreateTexture2D(dev, &td, NULL, &tex);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *) tex, NULL, &rtv);
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *q = NULL;
    ID3D11Device_CreateQuery(dev, &qd, &q);
    int ok = 0, lost = 0;
    double sum = 0, worst = 0;
    for (int i = 0; i < n; i++) {
        float c[4] = { (float) i / (float) n, 0.5f, 0.2f, 1.0f };
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, c);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) q);
        double t0 = now_ms();
        BOOL done = FALSE;
        for (;;) {
            hr = ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *) q, &done, sizeof done, 0);
            if (hr == S_OK) break;
            if (FAILED(hr) || now_ms() - t0 > 5000) break;
            Sleep(0);
        }
        double dt = now_ms() - t0;
        if (hr == S_OK && done) { ok++; sum += dt; if (dt > worst) worst = dt; }
        else lost++;
    }
    printf("EVENT のクエリ：%d 回中 %d 回返った・返らなかった %d 回・平均 %.2f ms・いちばん長い %.2f ms\n", n, ok, lost, ok ? sum / ok : 0.0, worst);
    return lost ? 2 : 0;
}
