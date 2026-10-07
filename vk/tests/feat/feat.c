// DirectX 11 のよく使う機能を 1 つずつ試す。窓を作らず、画面の外の的に描いて読み出す。
//   feat.exe <場面> <出す PPM>      feat.exe list で場面の一覧
// 絵は PPM に並べて出し、値は標準出力の「値 ...」の行に出す（比べる行）。「情報 ...」の行は比べない
#include "common.h"
#include "s_fb.c"
#include "s_tex.c"
#include "s_misc.c"

static void scene_msaa2(void) { scene_msaa(2); }
static void scene_msaa4(void) { scene_msaa(4); }

static const struct { const char *name; void (*fn)(void); } SCENES[] = {
    { "depth", scene_depth }, { "stencil", scene_stencil }, { "blend", scene_blend }, { "msaa2", scene_msaa2 }, { "msaa4", scene_msaa4 },
    { "mrt", scene_mrt }, { "texarr", scene_texarr }, { "cube", scene_cube }, { "mip", scene_mip }, { "sampler", scene_sampler },
    { "shadow", scene_shadow }, { "cbuf", scene_cbuf }, { "cs_uav", scene_cs_uav }, { "cs_atomic", scene_cs_atomic },
    { "cs_shared", scene_cs_shared }, { "cs_struct", scene_cs_struct }, { "cs_append", scene_cs_append }, { "indirect", scene_indirect },
    { "query", scene_query }, { "srgb", scene_srgb }, { "fmt", scene_fmt }, { "copy", scene_copy }, { "genmips", scene_genmips },
    { "raster", scene_raster }, { "vfmt", scene_vfmt }, { "deferred", scene_deferred }, { "dualsrc", scene_dualsrc },
    { "so", scene_so }, { "tess", scene_tess },
};

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    if (argc > 1 && !strcmp(argv[1], "list")) {
        for (size_t i = 0; i < sizeof SCENES / sizeof *SCENES; i++) printf("%s\n", SCENES[i].name);
        return 0;
    }
    if (argc < 3) { printf("feat <場面|list> <out.ppm>\n"); return 2; }
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    g_compile = dc ? (PFN_COMPILE) GetProcAddress(dc, "D3DCompile") : NULL;
    if (!g_compile) { printf("d3dcompiler_47.dll が無い\n"); return 1; }
    D3D_FEATURE_LEVEL want[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want, 2, D3D11_SDK_VERSION, &dev, &g_fl, &ctx);
    if (FAILED(hr)) hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, want + 1, 1, D3D11_SDK_VERSION, &dev, &g_fl, &ctx);
    if (FAILED(hr)) { printf("失敗 D3D11CreateDevice（0x%08lx）\n", (unsigned long) hr); return 1; }
    ID3D11Device_QueryInterface(dev, &IID_ID3D11Device1, (void **) &dev1);
    ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **) &ctx1);
    IDXGIDevice *xd = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC desc = { 0 };
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **) &xd)) && SUCCEEDED(IDXGIDevice_GetAdapter(xd, &ad))) IDXGIAdapter_GetDesc(ad, &desc);
    char name[256];
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof name, NULL, NULL);
    info("装置 %s・機能の段階 %x・D3D11.1 %s", name, (unsigned) g_fl, ctx1 ? "あり" : "なし");
    const char *s = argv[1];
    for (size_t i = 0; i < sizeof SCENES / sizeof *SCENES; i++)
        if (!strcmp(s, SCENES[i].name)) {
            SCENES[i].fn();
            save_ppm(argv[2]);
            ID3D11DeviceContext_Flush(ctx);
            HRESULT rr = ID3D11Device_GetDeviceRemovedReason(dev);
            if (rr != S_OK) printf("失敗 装置が外れた 0x%08lx\n", (unsigned long) rr);
            printf("終わり\n");
            fflush(stdout);
            return 0;
        }
    printf("知らない場面 %s\n", s);
    return 2;
}
