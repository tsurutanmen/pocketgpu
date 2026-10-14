// DXGI の出力（モニター）の一覧が取れるかを確かめる。GameMaker の DirectX 11 のゲームが起動のときにやること。
//   dxgiout.exe [sc]
//   1. CreateDXGIFactory1 → EnumAdapters1 の全部 → それぞれの EnumOutputs を取れるだけ → GetDesc と GetDisplayModeList
//   2. D3D11CreateDevice(既定のアダプター) → IDXGIDevice::GetAdapter → EnumOutputs(0) → GetDisplayModeList
//   3. sc を付けたときだけ、隠しウィンドウに交換鎖を作り、GetContainingOutput と GetDisplayModeList
// どこかで失敗すれば終了コード 1。出力が 1 つも無いときも 1
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(what, hr) do { HRESULT h_ = (hr); printf("  %-46s -> 0x%08lx %s\n", what, (unsigned long) h_, FAILED(h_) ? "NG" : "ok"); if (FAILED(h_)) g_fail = 1; } while (0)

static const struct { DXGI_FORMAT f; const char *n; } kFmt[] = {
    { DXGI_FORMAT_R8G8B8A8_UNORM, "R8G8B8A8_UNORM" },
    { DXGI_FORMAT_B8G8R8A8_UNORM, "B8G8R8A8_UNORM" },
    { DXGI_FORMAT_R10G10B10A2_UNORM, "R10G10B10A2_UNORM" },
    { DXGI_FORMAT_R16G16B16A16_FLOAT, "R16G16B16A16_FLOAT" },
};

static int dump_output(IDXGIOutput *out, int quiet_modes) {
    DXGI_OUTPUT_DESC od;
    HRESULT hr = IDXGIOutput_GetDesc(out, &od);
    CHECK("IDXGIOutput::GetDesc", hr);
    if (SUCCEEDED(hr))
        printf("    名前 %ls  座標 (%ld,%ld)-(%ld,%ld)  モニター %p\n", od.DeviceName, od.DesktopCoordinates.left, od.DesktopCoordinates.top,
               od.DesktopCoordinates.right, od.DesktopCoordinates.bottom, (void *) od.Monitor);
    int total = 0;
    for (unsigned i = 0; i < sizeof kFmt / sizeof kFmt[0]; i++) {
        UINT n = 0;
        char what[96];
        snprintf(what, sizeof what, "GetDisplayModeList(%s) 数", kFmt[i].n);
        hr = IDXGIOutput_GetDisplayModeList(out, kFmt[i].f, 0, &n, NULL);
        if (kFmt[i].f != DXGI_FORMAT_R8G8B8A8_UNORM && kFmt[i].f != DXGI_FORMAT_B8G8R8A8_UNORM && FAILED(hr)) {
            printf("  %-46s -> 0x%08lx（この形式は任意）\n", what, (unsigned long) hr);
            continue;
        }
        CHECK(what, hr);
        if (FAILED(hr) || n == 0) { if (SUCCEEDED(hr)) printf("    モード 0 件\n"); continue; }
        DXGI_MODE_DESC *m = calloc(n, sizeof *m);
        hr = IDXGIOutput_GetDisplayModeList(out, kFmt[i].f, 0, &n, m);
        snprintf(what, sizeof what, "GetDisplayModeList(%s) 取得", kFmt[i].n);
        CHECK(what, hr);
        if (SUCCEEDED(hr) && !quiet_modes) {
            printf("    %u 件。最初 %ux%u@%u/%u 最後 %ux%u@%u/%u\n", n, m[0].Width, m[0].Height, m[0].RefreshRate.Numerator, m[0].RefreshRate.Denominator,
                   m[n - 1].Width, m[n - 1].Height, m[n - 1].RefreshRate.Numerator, m[n - 1].RefreshRate.Denominator);
        }
        if (SUCCEEDED(hr) && kFmt[i].f == DXGI_FORMAT_R8G8B8A8_UNORM) {
            total = (int) n;
            DXGI_MODE_DESC want = m[n / 2], got;
            memset(&got, 0, sizeof got);
            hr = IDXGIOutput_FindClosestMatchingMode(out, &want, &got, NULL);
            CHECK("FindClosestMatchingMode", hr);
        }
        free(m);
    }
    return total;
}

static void dump_adapter_outputs(IDXGIAdapter *ad, const char *label) {
    printf("[%s] EnumOutputs\n", label);
    int found = 0;
    for (UINT o = 0;; o++) {
        IDXGIOutput *out = NULL;
        HRESULT hr = IDXGIAdapter_EnumOutputs(ad, o, &out);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            printf("  EnumOutputs(%u) -> DXGI_ERROR_NOT_FOUND（ここで終わり）\n", o);
            break;
        }
        char what[64];
        snprintf(what, sizeof what, "EnumOutputs(%u)", o);
        CHECK(what, hr);
        if (FAILED(hr)) break;
        found++;
        dump_output(out, 0);
        IDXGIOutput_Release(out);
        if (o > 16) break;
    }
    printf("  出力 %d 個\n", found);
    if (found == 0) g_fail = 1;
}


// 全部の DXGI 形式 × サンプル数で CheckMultisampleQualityLevels と CheckFormatSupport を呼び、S_OK 以外（と品質数）を並べる。
// DXVK は形式の問い合わせが Vulkan に断られると E_FAIL を返すので、GameMaker が起動のときに MSAA を調べる所で落ちる
static int msaa_scan(ID3D11Device *dev) {
    int bad = 0, total = 0;
    static const UINT counts[] = { 1, 2, 4, 8, 16 };
    for (UINT f = 1; f <= 132; f++) {
        UINT sup = 0;
        HRESULT hs = ID3D11Device_CheckFormatSupport(dev, (DXGI_FORMAT) f, &sup);
        printf("FMT %3u support 0x%08lx flags 0x%08x\n", f, (unsigned long) hs, sup);
        for (unsigned i = 0; i < sizeof counts / sizeof *counts; i++) {
            UINT q = 0xdead;
            HRESULT hr = ID3D11Device_CheckMultisampleQualityLevels(dev, (DXGI_FORMAT) f, counts[i], &q);
            total++;
            if (FAILED(hr)) bad++;
            printf("MSAA %3u x%-2u -> 0x%08lx q=%u\n", f, counts[i], (unsigned long) hr, hr == S_OK ? q : 0);
        }
    }
    printf("MSAA 走査：%d 回のうち失敗 %d\n", total, bad);
    return bad;
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    int want_sc = argc > 1 && !strcmp(argv[1], "sc");
    IDXGIFactory1 *fac = NULL;
    HRESULT hr = CreateDXGIFactory1(&IID_IDXGIFactory1, (void **) &fac);
    CHECK("CreateDXGIFactory1", hr);
    if (FAILED(hr)) return 1;
    printf("== 1. アダプターの一覧\n");
    for (UINT a = 0;; a++) {
        IDXGIAdapter1 *ad = NULL;
        hr = IDXGIFactory1_EnumAdapters1(fac, a, &ad);
        if (hr == DXGI_ERROR_NOT_FOUND) break;
        char what[64];
        snprintf(what, sizeof what, "EnumAdapters1(%u)", a);
        CHECK(what, hr);
        if (FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 d;
        IDXGIAdapter1_GetDesc1(ad, &d);
        printf("  %ls  vendor %04x device %04x  LUID %08lx:%08lx  VRAM %llu MB\n", d.Description, d.VendorId, d.DeviceId, (unsigned long) d.AdapterLuid.HighPart,
               (unsigned long) d.AdapterLuid.LowPart, (unsigned long long) (d.DedicatedVideoMemory >> 20));
        dump_adapter_outputs((IDXGIAdapter *) ad, "EnumAdapters1");
        IDXGIAdapter1_Release(ad);
    }
    IDXGIFactory1_Release(fac);

    printf("== 2a. EnumAdapters1(0) のアダプターを渡して D3D11CreateDevice（GameMaker のやり方。feature level の並びと BGRA 付き）\n");
    {
        IDXGIFactory1 *f2 = NULL;
        IDXGIAdapter1 *a2 = NULL;
        if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **) &f2)) && SUCCEEDED(IDXGIFactory1_EnumAdapters1(f2, 0, &a2))) {
            static const D3D_FEATURE_LEVEL fls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0, D3D_FEATURE_LEVEL_9_3, D3D_FEATURE_LEVEL_9_2, D3D_FEATURE_LEVEL_9_1 };
            ID3D11Device *d2 = NULL;
            D3D_FEATURE_LEVEL got;
            HRESULT h2 = D3D11CreateDevice((IDXGIAdapter *) a2, D3D_DRIVER_TYPE_UNKNOWN, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, fls, 6, D3D11_SDK_VERSION, &d2, &got, NULL);
            CHECK("D3D11CreateDevice(adapter, UNKNOWN, FL 並び)", h2);
            if (SUCCEEDED(h2)) { printf("    feature level 0x%x\n", (unsigned) got); ID3D11Device_Release(d2); }
        } else {
            printf("  EnumAdapters1(0) が取れない\n");
            g_fail = 1;
        }
        if (a2) IDXGIAdapter1_Release(a2);
        if (f2) IDXGIFactory1_Release(f2);
    }
    printf("== 2. D3D11CreateDevice → IDXGIDevice::GetAdapter\n");
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    D3D_FEATURE_LEVEL fl;
    hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx);
    CHECK("D3D11CreateDevice", hr);
    if (FAILED(hr)) return 1;
    IDXGIDevice *dd = NULL;
    IDXGIAdapter *ad = NULL;
    hr = ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **) &dd);
    CHECK("QueryInterface(IDXGIDevice)", hr);
    hr = IDXGIDevice_GetAdapter(dd, &ad);
    CHECK("IDXGIDevice::GetAdapter", hr);
    if (SUCCEEDED(hr)) {
        DXGI_ADAPTER_DESC d;
        IDXGIAdapter_GetDesc(ad, &d);
        printf("  %ls  LUID %08lx:%08lx\n", d.Description, (unsigned long) d.AdapterLuid.HighPart, (unsigned long) d.AdapterLuid.LowPart);
        dump_adapter_outputs(ad, "device の adapter");
    }

    if (argc > 1 && !strcmp(argv[1], "msaa")) { msaa_scan(dev); return 0; }
    if (want_sc && ad) {
        printf("== 3. 隠しウィンドウの交換鎖\n");
        WNDCLASSA wc = { 0 };
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "dxgiout";
        RegisterClassA(&wc);
        HWND w = CreateWindowA("dxgiout", "dxgiout", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, NULL, NULL, wc.hInstance, NULL);
        IDXGIFactory *pf = NULL;
        hr = IDXGIAdapter_GetParent(ad, &IID_IDXGIFactory, (void **) &pf);
        CHECK("IDXGIAdapter::GetParent(IDXGIFactory)", hr);
        DXGI_SWAP_CHAIN_DESC sd = { 0 };
        sd.BufferDesc.Width = 320;
        sd.BufferDesc.Height = 240;
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = 2;
        sd.OutputWindow = w;
        sd.Windowed = TRUE;
        sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGISwapChain *sc = NULL;
        if (pf) {
            hr = IDXGIFactory_CreateSwapChain(pf, (IUnknown *) dev, &sd, &sc);
            CHECK("CreateSwapChain", hr);
        }
        if (sc) {
            IDXGIOutput *out = NULL;
            hr = IDXGISwapChain_GetContainingOutput(sc, &out);
            CHECK("IDXGISwapChain::GetContainingOutput", hr);
            if (out) { dump_output(out, 1); IDXGIOutput_Release(out); }
            IDXGISwapChain_Release(sc);
        }
        if (pf) IDXGIFactory_Release(pf);
        DestroyWindow(w);
    }
    if (ad) IDXGIAdapter_Release(ad);
    if (dd) IDXGIDevice_Release(dd);
    ID3D11DeviceContext_Release(ctx);
    ID3D11Device_Release(dev);
    printf(g_fail ? "結果：NG\n" : "結果：OK\n");
    return g_fail;
}
