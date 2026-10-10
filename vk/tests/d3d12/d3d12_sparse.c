// DirectX 12 の tiled resources（reserved resource）を本当に使う試験。kit_extra\research\sparse_emulation.md の S1 を確かめる物。
//   d3d12_sparse.exe [場面|all]      場面：caps・buf・tex・promise（省くと all）
//   caps   ：feature level・TiledResourcesTier・reserved のテクスチャのタイルの形（GetResourceTiling）を出す。作れなければ違う
//   buf    ：reserved の buffer（4 タイル）の 1 番と 3 番だけをヒープに結び、CPU から写した値と GPU が書いた値を
//            写し出し（CopyBufferRegion）と、状態つきの読み（ByteAddressBuffer.Load の status）で確かめる
//   tex    ：reserved の 256x256 のテクスチャ（2x2 タイル）の左上と右下だけを結び、全画面の三角形で描いて読み戻す。
//            計算シェーダーで状態つきの Load と SampleLevel でも読む（dxil や dxbc の sparse の読みになる）
//   promise：tiled resources の約束（結んでいない所は 0・CheckAccessFullyMapped が偽・同じヒープのタイルを 2 か所に結ぶと同じ中身・
//            結び直すと中身がついて来る・外すと 0）。S1 は守らない。違っても「違う」には数えず「約束 …：破る」と書く
//   big    ：16384x16384・15 段（約 1.3 GiB）の reserved のテクスチャを作り、1 タイルだけ結んで写して読み戻す。
//            S1 はメモリの上限（標準 1024 MiB）で作らずに断る。作れないのは「約束 …：破る」と書く
// 結んだ所の値だけを「正しい／違う」に数える。最後の行は「結果 全部正しい（約束を破った所 n）」か「結果 n 件違う」。
// D3D12_SPARSE_STRICT=1 で、約束を破った所も「違う」に数える（ふつうの DirectX 12 では全部守るはず）。
// D3D12_SPARSE_DEBUG=1 で調べる層（ふつうの DirectX 12 だけ）。
// D3D12_SPARSE_BUFVIEW=raw|typed|struct で buf の状態つきの読みの見え方を選ぶ（標準 typed）。3060 のふつうの DirectX 12 では、
//   raw と structured の状態つきの読みは、結んだタイルでも CheckAccessFullyMapped が偽になる（値は正しい）。typed だけ真になるので typed を標準にした。
// 組み方（app\vk\build_win.sh と同じ指定）：
//   export PATH="/d/tools/zig-x86_64-windows-0.17.0:$PATH"
//   zig cc -target x86_64-windows-gnu -O2 -g -Wall -Wextra -Wno-unused-parameter d3d12_sparse.c -o d3d12_sparse.exe
// 代役で走らせる台本は work\sparse\run.sh（例：bash run.sh build s1 d3d12_sparse.exe all）。
// 書き方は app\vk\tests\d3d12\d3d12test.c と kit_extra\tests\d3d12_rootcbv.c に合わせた。横に d3d12.dll（vkd3d-proton）を置けばそれを、無ければ Windows の DirectX 12 を使う。
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef HRESULT (WINAPI *PFN_COMPILE)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);
static PFN_COMPILE g_compile;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE g_serialize;
static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence;
static UINT64 fence_val;
static HANDLE fence_ev;
static unsigned g_wrong;  // 違った数（場面ごと）
static unsigned g_broken; // 約束を破った数（全体）
static int g_strict;

enum { TILE = 65536 };

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("失敗 ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    if (dev) {
        HRESULT rr = ID3D12Device_GetDeviceRemovedReason(dev);
        if (rr != S_OK) printf("失敗 装置が外れた 0x%08lx\n", (unsigned long) rr);
    }
    printf("結果 まちがいあり\n");
    fflush(stdout);
    exit(3);
}
#define CK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) die("%s = 0x%08lx（%d 行）", #x, (unsigned long) hr_, __LINE__); } while (0)

static void info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("情報 ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}
static void val(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("値 ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}
// 結んだ所の確かめ。違えば数えて、はじめの 8 つだけ書く
static void expect(const char *what, uint32_t got, uint32_t want) {
    if (got == want) return;
    if (g_wrong < 8) info("違う：%s %08x（%08x のはず）", what, got, want);
    g_wrong++;
}
// 約束の確かめ。守らなくても「違う」には数えない（D3D12_SPARSE_STRICT=1 なら数える）
static void promise(const char *what, int kept, const char *detail) {
    printf("約束 %s：%s（%s）\n", what, kept ? "守る" : "破る", detail);
    fflush(stdout);
    if (!kept) {
        g_broken++;
        if (g_strict) g_wrong++;
    }
}

// ---- 仕事を送って待つ ----
static void wait_idle(void) {
    fence_val++;
    CK(ID3D12CommandQueue_Signal(queue, fence, fence_val));
    if (ID3D12Fence_GetCompletedValue(fence) < fence_val) {
        CK(ID3D12Fence_SetEventOnCompletion(fence, fence_val, fence_ev));
        if (WaitForSingleObject(fence_ev, 30000) != WAIT_OBJECT_0) die("30 秒待っても終わらない");
    }
}
static void run_and_wait(void) {
    CK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandList *l = (ID3D12CommandList *) cl;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, &l);
    wait_idle();
    CK(ID3D12CommandAllocator_Reset(alloc));
    CK(ID3D12GraphicsCommandList_Reset(cl, alloc, NULL));
}

// ---- シェーダーと root signature ----
static ID3DBlob *compile(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = g_compile(src, strlen(src), "d3d12_sparse.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
    if (FAILED(hr)) die("シェーダーを組めない（%s %s）：%s", entry, target, err ? (const char *) ID3D10Blob_GetBufferPointer(err) : "?");
    return code;
}
static D3D12_SHADER_BYTECODE bc(ID3DBlob *b) { return (D3D12_SHADER_BYTECODE) { ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b) }; }

static ID3D12RootSignature *mk_rs(const D3D12_ROOT_SIGNATURE_DESC *d) {
    ID3DBlob *blob = NULL, *err = NULL;
    HRESULT hr = g_serialize(d, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err);
    if (FAILED(hr)) die("root signature を作れない 0x%08lx：%s", (unsigned long) hr, err ? (const char *) ID3D10Blob_GetBufferPointer(err) : "?");
    ID3D12RootSignature *rs = NULL;
    CK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob), &IID_ID3D12RootSignature, (void **) &rs));
    ID3D10Blob_Release(blob);
    return rs;
}
// 計算用の root signature：表 0 は SRV t0、表 1 は UAV u0・u1、b0 は定数 4 つ、s0 は点で読む sampler
static ID3D12RootSignature *mk_cs_rs(void) {
    static D3D12_DESCRIPTOR_RANGE srv = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    static D3D12_DESCRIPTOR_RANGE uav = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER p[3] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &srv }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &uav }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 0, 0, 4 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_STATIC_SAMPLER_DESC ss = { .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT, .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                     .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                     .MaxLOD = D3D12_FLOAT32_MAX, .ShaderRegister = 0, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL };
    D3D12_ROOT_SIGNATURE_DESC rd = { 3, p, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    return mk_rs(&rd);
}
static ID3D12PipelineState *mk_cs(ID3D12RootSignature *rs, const char *src) {
    ID3DBlob *cs = compile(src, "cs", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { .pRootSignature = rs, .CS = bc(cs) };
    ID3D12PipelineState *pso = NULL;
    CK(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **) &pso));
    ID3D10Blob_Release(cs);
    return pso;
}

// ---- 物を作る ----
static ID3D12Resource *mk_buf(UINT64 size, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp = { .Type = ht };
    D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
                              .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = fl };
    ID3D12Resource *r = NULL;
    CK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **) &r));
    return r;
}
static ID3D12Heap *mk_tile_heap(UINT tiles, D3D12_HEAP_FLAGS fl) {
    D3D12_HEAP_DESC hd = { .SizeInBytes = (UINT64) tiles * TILE, .Properties = { .Type = D3D12_HEAP_TYPE_DEFAULT },
                           .Alignment = D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT, .Flags = fl };
    ID3D12Heap *h = NULL;
    CK(ID3D12Device_CreateHeap(dev, &hd, &IID_ID3D12Heap, (void **) &h));
    return h;
}
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                 .Transition = { .pResource = r, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, .StateBefore = a, .StateAfter = b } };
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &x);
}
static void uav_barrier(ID3D12Resource *r) {
    D3D12_RESOURCE_BARRIER x = { .Type = D3D12_RESOURCE_BARRIER_TYPE_UAV, .UAV = { .pResource = r } };
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &x);
}
static ID3D12DescriptorHeap *mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE t, UINT n, int visible) {
    D3D12_DESCRIPTOR_HEAP_DESC d = { .Type = t, .NumDescriptors = n, .Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    ID3D12DescriptorHeap *h = NULL;
    CK(ID3D12Device_CreateDescriptorHeap(dev, &d, &IID_ID3D12DescriptorHeap, (void **) &h));
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(ID3D12DescriptorHeap *h, UINT i, D3D12_DESCRIPTOR_HEAP_TYPE t) {
    D3D12_CPU_DESCRIPTOR_HANDLE c = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h);
    c.ptr += (SIZE_T) i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, t);
    return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(ID3D12DescriptorHeap *h, UINT i) {
    D3D12_GPU_DESCRIPTOR_HANDLE g = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h);
    g.ptr += (UINT64) i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return g;
}
#define CSU D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV
static void srv_raw(ID3D12Resource *b, UINT64 size, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = { .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                           .Buffer = { .FirstElement = 0, .NumElements = (UINT) (size / 4), .Flags = D3D12_BUFFER_SRV_FLAG_RAW } };
    ID3D12Device_CreateShaderResourceView(dev, b, &sd, h);
}
static void uav_raw(ID3D12Resource *b, UINT64 size, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = { .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
                                            .Buffer = { .FirstElement = 0, .NumElements = (UINT) (size / 4), .Flags = D3D12_BUFFER_UAV_FLAG_RAW } };
    ID3D12Device_CreateUnorderedAccessView(dev, b, NULL, &ud, h);
}
static void uav_struct(ID3D12Resource *b, UINT n, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = { .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
                                            .Buffer = { .FirstElement = 0, .NumElements = n, .StructureByteStride = 4 } };
    ID3D12Device_CreateUnorderedAccessView(dev, b, NULL, &ud, h);
}

// buffer を読み出し用の buffer に写して読む。終わったら free で返す
static uint32_t *read_buf(ID3D12Resource *b, UINT64 size, D3D12_RESOURCE_STATES st) {
    ID3D12Resource *rb = mk_buf(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(b, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, rb, 0, b, 0, size);
    barrier(b, D3D12_RESOURCE_STATE_COPY_SOURCE, st);
    run_and_wait();
    void *p = NULL;
    D3D12_RANGE all = { 0, (SIZE_T) size };
    CK(ID3D12Resource_Map(rb, 0, &all, &p));
    uint32_t *out = malloc((size_t) size);
    memcpy(out, p, (size_t) size);
    ID3D12Resource_Unmap(rb, 0, NULL);
    ID3D12Resource_Release(rb);
    return out;
}

// reserved の物を作る
static ID3D12Resource *mk_reserved(const D3D12_RESOURCE_DESC *d, D3D12_RESOURCE_STATES st, HRESULT *hr_out) {
    ID3D12Resource *r = NULL;
    HRESULT hr = ID3D12Device_CreateReservedResource(dev, d, st, NULL, &IID_ID3D12Resource, (void **) &r);
    if (hr_out) *hr_out = hr;
    return SUCCEEDED(hr) ? r : NULL;
}
// 1 タイルずつ結ぶ（heap_tile が ~0u なら外す）
static void map_tile(ID3D12Resource *r, UINT x, UINT y, UINT sub, ID3D12Heap *h, UINT heap_tile) {
    D3D12_TILED_RESOURCE_COORDINATE c = { x, y, 0, sub };
    D3D12_TILE_REGION_SIZE s = { 1, FALSE, 0, 0, 0 };
    D3D12_TILE_RANGE_FLAGS f = heap_tile == ~0u ? D3D12_TILE_RANGE_FLAG_NULL : D3D12_TILE_RANGE_FLAG_NONE;
    UINT off = heap_tile == ~0u ? 0 : heap_tile, cnt = 1;
    ID3D12CommandQueue_UpdateTileMappings(queue, r, 1, &c, &s, heap_tile == ~0u ? NULL : h, 1, &f, &off, &cnt, D3D12_TILE_MAPPING_FLAG_NONE);
}

// ---- 場面 caps ----
static int g_tier;
static void scene_caps(void) {
    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_12_2, D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { 5, lv, 0 };
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl)))
        val("feature level の最高 %x_%x", (fl.MaxSupportedFeatureLevel >> 12) & 0xf, (fl.MaxSupportedFeatureLevel >> 8) & 0xf);
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = { 0 };
    CK(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o));
    g_tier = o.TiledResourcesTier;
    val("TiledResourcesTier %d・ResourceHeapTier %d", o.TiledResourcesTier, o.ResourceHeapTier);
    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM };
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof fs)))
        val("R8G8B8A8_UNORM の tiled：%s", (fs.Support2 & D3D12_FORMAT_SUPPORT2_TILED) ? "あり" : "なし");
    if (!o.TiledResourcesTier) { info("違う：TiledResourcesTier が 0（tiled resources が無い）"); g_wrong++; return; }
    // 1024x1024・11 段のテクスチャのタイルの形と、まとめて持つ小さい段（packed mips）
    D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = 1024, .Height = 1024, .DepthOrArraySize = 1, .MipLevels = 11,
                              .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE };
    HRESULT hr;
    ID3D12Resource *t = mk_reserved(&d, D3D12_RESOURCE_STATE_COPY_DEST, &hr);
    if (!t) { info("違う：1024x1024 の reserved のテクスチャを作れない 0x%08lx", (unsigned long) hr); g_wrong++; return; }
    UINT total = 0, nsub = 11;
    D3D12_PACKED_MIP_INFO pm = { 0 };
    D3D12_TILE_SHAPE shape = { 0 };
    D3D12_SUBRESOURCE_TILING st[11];
    memset(st, 0, sizeof st);
    ID3D12Device_GetResourceTiling(dev, (ID3D12Resource *) t, &total, &pm, &shape, &nsub, 0, st);
    val("1024x1024・11 段：タイル全部 %u・形 %ux%ux%u・ふつうの段 %u・まとめた段 %u（タイル %u・%u 番から）", total, shape.WidthInTexels, shape.HeightInTexels,
        shape.DepthInTexels, pm.NumStandardMips, pm.NumPackedMips, pm.NumTilesForPackedMips, pm.StartTileIndexInOverallResource);
    for (UINT i = 0; i < nsub && i < 4; i++)
        val("  段 %u：%ux%ux%u タイル・%u 番から", i, st[i].WidthInTiles, st[i].HeightInTiles, st[i].DepthInTiles, st[i].StartTileIndexInOverallResource);
    if (shape.WidthInTexels != 128 || shape.HeightInTexels != 128) { info("違う：32 ビットの形のタイルが 128x128 でない"); g_wrong++; }
    ID3D12Resource_Release(t);
}

// ---- 場面 buf ----
// タイル t の i 語目：CPU から写す値（前半）と、GPU が書く値（後半）
static uint32_t cpu_word(uint32_t t, uint32_t i) { return 0xA0000000u | (t << 20) | i; }
static uint32_t gpu_word(uint32_t t, uint32_t i) { return 0xB0000000u | (t << 20) | i; }
enum { WORDS = TILE / 4, HALF = WORDS / 2 };
static const char *BUF_WRITE_HLSL =
    "RWByteAddressBuffer RB : register(u1);\n"
    "[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    uint w = id.x, t = w >> 14, i = w & 16383;\n"
    "    if (i >= 8192) RB.Store(w * 4, 0xB0000000u | (t << 20) | i);\n"
    "}\n";
// 状態つきの読み：タイルごとに前半と後半の 5 語目。D3D12_SPARSE_BUFVIEW=raw|typed|struct で読み方を選ぶ（標準 typed）
static const char *BUF_READ_RAW_HLSL =
    "ByteAddressBuffer RB : register(t0);\n"
    "RWStructuredBuffer<uint> O : register(u0);\n"
    "[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    for (uint t = 0; t < 4; t++) for (uint h = 0; h < 2; h++) {\n"
    "        uint st; uint v = RB.Load((t * 16384 + h * 8192 + 5) * 4, st);\n"
    "        uint k = t * 2 + h;\n"
    "        O[k * 2] = v; O[k * 2 + 1] = CheckAccessFullyMapped(st) ? 1 : 0;\n"
    "    }\n"
    "}\n";
static const char *BUF_READ_TYPED_HLSL =
    "Buffer<uint> RB : register(t0);\n"
    "RWStructuredBuffer<uint> O : register(u0);\n"
    "[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    for (uint t = 0; t < 4; t++) for (uint h = 0; h < 2; h++) {\n"
    "        uint st; uint v = RB.Load(t * 16384 + h * 8192 + 5, st);\n"
    "        uint k = t * 2 + h;\n"
    "        O[k * 2] = v; O[k * 2 + 1] = CheckAccessFullyMapped(st) ? 1 : 0;\n"
    "    }\n"
    "}\n";
static const char *BUF_READ_STRUCT_HLSL =
    "StructuredBuffer<uint> RB : register(t0);\n"
    "RWStructuredBuffer<uint> O : register(u0);\n"
    "[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    for (uint t = 0; t < 4; t++) for (uint h = 0; h < 2; h++) {\n"
    "        uint st; uint v = RB.Load(t * 16384 + h * 8192 + 5, st);\n"
    "        uint k = t * 2 + h;\n"
    "        O[k * 2] = v; O[k * 2 + 1] = CheckAccessFullyMapped(st) ? 1 : 0;\n"
    "    }\n"
    "}\n";
static int g_bufview; // 0 raw・1 typed・2 struct

static ID3D12Resource *g_rbuf;   // promise の場面でも使う
static ID3D12Heap *g_bheap;
static ID3D12RootSignature *g_cs_rs;

static void scene_buf(void) {
    if (!g_cs_rs) g_cs_rs = mk_cs_rs();
    D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = 4 * TILE, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
                              .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                              .Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS };
    HRESULT hr;
    ID3D12Resource *rb = mk_reserved(&d, D3D12_RESOURCE_STATE_COMMON, &hr);
    if (!rb) { info("違う：reserved の buffer を作れない 0x%08lx", (unsigned long) hr); g_wrong++; return; }
    g_rbuf = rb;
    g_bheap = mk_tile_heap(2, D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS);
    map_tile(rb, 1, 0, 0, g_bheap, 0);
    map_tile(rb, 3, 0, 0, g_bheap, 1);
    // CPU の値を 1 番と 3 番の前半に写す
    ID3D12Resource *up = mk_buf(4 * TILE, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint32_t *p = NULL;
    D3D12_RANGE none = { 0, 0 };
    CK(ID3D12Resource_Map(up, 0, &none, (void **) &p));
    for (uint32_t t = 0; t < 4; t++)
        for (uint32_t i = 0; i < WORDS; i++) p[t * WORDS + i] = cpu_word(t, i);
    ID3D12Resource_Unmap(up, 0, NULL);
    barrier(rb, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, rb, 1 * TILE, up, 1 * TILE, TILE / 2);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, rb, 3 * TILE, up, 3 * TILE, TILE / 2);
    barrier(rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // GPU が全部のタイルの後半に書く（結んでいない所への書き込みは捨てられるはず）
    ID3D12DescriptorHeap *h = mk_heap(CSU, 3, 1);
    ID3D12Resource *out = mk_buf(256 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (g_bufview == 0) srv_raw(rb, 4 * TILE, cpu_at(h, 0, CSU));
    else {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = { .Format = g_bufview == 1 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                               .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                               .Buffer = { .FirstElement = 0, .NumElements = 4 * WORDS, .StructureByteStride = g_bufview == 2 ? 4 : 0 } };
        ID3D12Device_CreateShaderResourceView(dev, rb, &sd, cpu_at(h, 0, CSU));
    }
    uav_struct(out, 256, cpu_at(h, 1, CSU));
    uav_raw(rb, 4 * TILE, cpu_at(h, 2, CSU));
    ID3D12PipelineState *pw = mk_cs(g_cs_rs, BUF_WRITE_HLSL),
                        *pr = mk_cs(g_cs_rs, g_bufview == 0 ? BUF_READ_RAW_HLSL : g_bufview == 1 ? BUF_READ_TYPED_HLSL : BUF_READ_STRUCT_HLSL);
    val("buf：状態つきの読みは %s の見え方", g_bufview == 0 ? "raw" : g_bufview == 1 ? "typed（R32_UINT）" : "structured");
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &h);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, g_cs_rs);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 0, gpu_at(h, 0));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 1, gpu_at(h, 1));
    ID3D12GraphicsCommandList_SetPipelineState(cl, pw);
    ID3D12GraphicsCommandList_Dispatch(cl, 4 * WORDS / 64, 1, 1);
    uav_barrier(rb);
    barrier(rb, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pr);
    ID3D12GraphicsCommandList_Dispatch(cl, 1, 1, 1);
    barrier(rb, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    run_and_wait();
    // 写し出しで全部の語を比べる（結んだ 1 番と 3 番）
    uint32_t *all = read_buf(rb, 4 * TILE, D3D12_RESOURCE_STATE_COMMON);
    unsigned bad = 0;
    for (uint32_t t = 1; t < 4; t += 2)
        for (uint32_t i = 0; i < WORDS; i++) {
            unsigned before = g_wrong;
            char what[64];
            snprintf(what, sizeof what, "写し出し タイル %u の %u 語目", t, i);
            expect(what, all[t * WORDS + i], i < HALF ? cpu_word(t, i) : gpu_word(t, i));
            bad += g_wrong != before;
        }
    val("buf：結んだ 2 タイルの %u 語を写し出しで比べた・違う %u 語", 2 * WORDS, bad);
    // 状態つきの読み
    uint32_t *o = read_buf(out, 256 * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    for (uint32_t t = 0; t < 4; t++)
        for (uint32_t hh = 0; hh < 2; hh++) {
            uint32_t k = t * 2 + hh, v = o[k * 2], mapped = o[k * 2 + 1];
            uint32_t want = hh ? gpu_word(t, HALF + 5) : cpu_word(t, 5);
            if (t == 1 || t == 3) {
                char what[64];
                snprintf(what, sizeof what, "Load タイル %u %s の値", t, hh ? "後半" : "前半");
                expect(what, v, want);
                snprintf(what, sizeof what, "Load タイル %u %s の CheckAccessFullyMapped", t, hh ? "後半" : "前半");
                expect(what, mapped, 1);
            } else {
                char what[96];
                snprintf(what, sizeof what, "結んでいないタイル %u %s の読み", t, hh ? "後半" : "前半");
                char det[96];
                snprintf(det, sizeof det, "Load %08x・CheckAccessFullyMapped %u・写し出し %08x", v, mapped, all[t * WORDS + (hh ? HALF : 0) + 5]);
                promise(what, v == 0 && mapped == 0 && all[t * WORDS + (hh ? HALF : 0) + 5] == 0, det);
            }
        }
    free(o);
    free(all);
    ID3D12PipelineState_Release(pw);
    ID3D12PipelineState_Release(pr);
    ID3D12Resource_Release(out);
    ID3D12Resource_Release(up);
    ID3D12DescriptorHeap_Release(h);
}

// ---- 場面 tex ----
static const char *TEX_DRAW_HLSL =
    "float4 vs(uint id : SV_VertexID) : SV_Position {\n"
    "    float2 p = float2((id << 1) & 2, id & 2);\n"
    "    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "}\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target {\n"
    "    uint x = (uint) pos.x, y = (uint) pos.y;\n"
    "    return float4(x / 255.0, y / 255.0, ((x + y) & 255) / 255.0, 1);\n"
    "}\n";
static const char *TEX_READ_HLSL =
    "Texture2D<float4> T : register(t0);\n"
    "SamplerState S : register(s0);\n"
    "RWStructuredBuffer<uint> O : register(u0);\n"
    "static const int2 P[4] = { int2(10, 20), int2(138, 20), int2(10, 148), int2(138, 148) };\n"
    "uint pk(float4 c) { uint4 u = uint4(round(saturate(c) * 255)); return u.x | (u.y << 8) | (u.z << 16) | (u.w << 24); }\n"
    "[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    for (uint k = 0; k < 4; k++) {\n"
    "        uint s1; float4 a = T.Load(int3(P[k], 0), int2(0, 0), s1);\n"
    "        uint s2; float4 b = T.SampleLevel(S, (float2(P[k]) + 0.5) / 256.0, 0, int2(0, 0), s2);\n"
    "        O[k * 4] = pk(a); O[k * 4 + 1] = CheckAccessFullyMapped(s1) ? 1 : 0;\n"
    "        O[k * 4 + 2] = pk(b); O[k * 4 + 3] = CheckAccessFullyMapped(s2) ? 1 : 0;\n"
    "    }\n"
    "}\n";
static uint32_t px_want(uint32_t x, uint32_t y) { return x | (y << 8) | (((x + y) & 255) << 16) | 0xFF000000u; }

static ID3D12Resource *g_rtex;
static ID3D12Heap *g_theap;

static void scene_tex(void) {
    if (!g_cs_rs) g_cs_rs = mk_cs_rs();
    enum { W = 256, H = 256 };
    D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = W, .Height = H, .DepthOrArraySize = 1, .MipLevels = 1,
                              .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE,
                              .Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
    HRESULT hr;
    ID3D12Resource *t = mk_reserved(&d, D3D12_RESOURCE_STATE_RENDER_TARGET, &hr);
    if (!t) { info("違う：reserved のテクスチャを作れない 0x%08lx", (unsigned long) hr); g_wrong++; return; }
    g_rtex = t;
    UINT total = 0, nsub = 1;
    D3D12_PACKED_MIP_INFO pm = { 0 };
    D3D12_TILE_SHAPE shape = { 0 };
    D3D12_SUBRESOURCE_TILING st = { 0 };
    ID3D12Device_GetResourceTiling(dev, (ID3D12Resource *) t, &total, &pm, &shape, &nsub, 0, &st);
    val("tex：256x256 のタイル %u（%ux%u）・形 %ux%u・まとめた段 %u", total, st.WidthInTiles, st.HeightInTiles, shape.WidthInTexels, shape.HeightInTexels, pm.NumPackedMips);
    if (st.WidthInTiles != 2 || st.HeightInTiles != 2) { info("違う：タイルが 2x2 に並ばない"); g_wrong++; return; }
    g_theap = mk_tile_heap(2, D3D12_HEAP_FLAG_ALLOW_ONLY_RT_DS_TEXTURES);
    map_tile(t, 0, 0, 0, g_theap, 0); // 左上
    map_tile(t, 1, 1, 0, g_theap, 1); // 右下
    // 描く
    D3D12_ROOT_SIGNATURE_DESC grd = { 0 };
    ID3D12RootSignature *grs = mk_rs(&grd);
    ID3DBlob *vs = compile(TEX_DRAW_HLSL, "vs", "vs_5_0"), *ps = compile(TEX_DRAW_HLSL, "ps", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = { 0 };
    g.pRootSignature = grs;
    g.VS = bc(vs);
    g.PS = bc(ps);
    g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    g.SampleMask = UINT_MAX;
    g.RasterizerState = (D3D12_RASTERIZER_DESC) { .FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_NONE, .DepthClipEnable = TRUE };
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    g.SampleDesc.Count = 1;
    ID3D12PipelineState *gp = NULL;
    CK(ID3D12Device_CreateGraphicsPipelineState(dev, &g, &IID_ID3D12PipelineState, (void **) &gp));
    ID3D12DescriptorHeap *rtvh = mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_at(rtvh, 0, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    ID3D12Device_CreateRenderTargetView(dev, t, NULL, rtv);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, grs);
    ID3D12GraphicsCommandList_SetPipelineState(cl, gp);
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    // 計算シェーダーで状態つきで読む
    barrier(t, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap *h = mk_heap(CSU, 3, 1);
    ID3D12Resource *out = mk_buf(256 * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = { .MipLevels = 1 } };
    ID3D12Device_CreateShaderResourceView(dev, t, &sd, cpu_at(h, 0, CSU));
    uav_struct(out, 256, cpu_at(h, 1, CSU));
    uav_struct(out, 256, cpu_at(h, 2, CSU));
    ID3D12PipelineState *pr = mk_cs(g_cs_rs, TEX_READ_HLSL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &h);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, g_cs_rs);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 0, gpu_at(h, 0));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 1, gpu_at(h, 1));
    ID3D12GraphicsCommandList_SetPipelineState(cl, pr);
    ID3D12GraphicsCommandList_Dispatch(cl, 1, 1, 1);
    // 読み戻す
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 tot = 0;
    ID3D12Device_GetCopyableFootprints(dev, &d, 0, 1, 0, &fp, NULL, NULL, &tot);
    ID3D12Resource *rbk = mk_buf(tot, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(t, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = { .pResource = rbk, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = fp };
    D3D12_TEXTURE_COPY_LOCATION src = { .pResource = t, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    barrier(t, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    run_and_wait();
    uint8_t *px = NULL;
    D3D12_RANGE all = { 0, (SIZE_T) tot };
    CK(ID3D12Resource_Map(rbk, 0, &all, (void **) &px));
    unsigned bad = 0;
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            int mapped = (x < 128 && y < 128) || (x >= 128 && y >= 128);
            if (!mapped) continue;
            uint32_t v;
            memcpy(&v, px + fp.Offset + (size_t) y * fp.Footprint.RowPitch + x * 4, 4);
            char what[64];
            snprintf(what, sizeof what, "描いた画素 (%u,%u)", x, y);
            unsigned before = g_wrong;
            expect(what, v, px_want(x, y));
            bad += g_wrong != before;
        }
    val("tex：結んだ 2 タイルの %u 画素を比べた・違う %u 画素", 2 * 128 * 128, bad);
    uint32_t un[2];
    memcpy(&un[0], px + fp.Offset + (size_t) 20 * fp.Footprint.RowPitch + 138 * 4, 4);
    memcpy(&un[1], px + fp.Offset + (size_t) 148 * fp.Footprint.RowPitch + 10 * 4, 4);
    ID3D12Resource_Unmap(rbk, 0, NULL);
    uint32_t *o = read_buf(out, 256 * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    static const uint32_t P[4][2] = { { 10, 20 }, { 138, 20 }, { 10, 148 }, { 138, 148 } };
    for (int k = 0; k < 4; k++) {
        int mapped = k == 0 || k == 3;
        char what[96];
        if (mapped) {
            snprintf(what, sizeof what, "Load (%u,%u) の値", P[k][0], P[k][1]);
            expect(what, o[k * 4], px_want(P[k][0], P[k][1]));
            snprintf(what, sizeof what, "Load (%u,%u) の CheckAccessFullyMapped", P[k][0], P[k][1]);
            expect(what, o[k * 4 + 1], 1);
            snprintf(what, sizeof what, "SampleLevel (%u,%u) の値", P[k][0], P[k][1]);
            expect(what, o[k * 4 + 2], px_want(P[k][0], P[k][1]));
            snprintf(what, sizeof what, "SampleLevel (%u,%u) の CheckAccessFullyMapped", P[k][0], P[k][1]);
            expect(what, o[k * 4 + 3], 1);
        } else {
            snprintf(what, sizeof what, "結んでいないタイルの画素 (%u,%u) の読み", P[k][0], P[k][1]);
            char det[128];
            uint32_t cp = k == 1 ? un[0] : un[1];
            snprintf(det, sizeof det, "Load %08x・%u／SampleLevel %08x・%u／写し出し %08x", o[k * 4], o[k * 4 + 1], o[k * 4 + 2], o[k * 4 + 3], cp);
            promise(what, o[k * 4] == 0 && o[k * 4 + 1] == 0 && o[k * 4 + 2] == 0 && o[k * 4 + 3] == 0 && cp == 0, det);
        }
    }
    free(o);
    ID3D12Resource_Release(rbk);
    ID3D12Resource_Release(out);
    ID3D12PipelineState_Release(pr);
    ID3D12PipelineState_Release(gp);
    ID3D12RootSignature_Release(grs);
    ID3D12DescriptorHeap_Release(h);
    ID3D12DescriptorHeap_Release(rtvh);
    ID3D10Blob_Release(vs);
    ID3D10Blob_Release(ps);
}

// ---- 場面 promise：結び直し・2 か所に結ぶ・外す（buf の場面のあとで） ----
static void scene_promise(void) {
    if (!g_rbuf) { info("buf の場面が先に要る（buf を作れなかった）"); g_wrong++; return; }
    // 0 番を、1 番と同じヒープのタイル 0 に結ぶ。同じ中身が見えるはず
    map_tile(g_rbuf, 0, 0, 0, g_bheap, 0);
    // 3 番を外す。0 が見えるはず
    map_tile(g_rbuf, 3, 0, 0, g_bheap, ~0u);
    // 2 番を、3 番が使っていたヒープのタイル 1 に結ぶ。3 番に書いた中身がついて来るはず
    map_tile(g_rbuf, 2, 0, 0, g_bheap, 1);
    uint32_t *all = read_buf(g_rbuf, 4 * TILE, D3D12_RESOURCE_STATE_COMMON);
    char det[96];
    snprintf(det, sizeof det, "0 番の 5 語目 %08x・1 番の 5 語目 %08x", all[5], all[WORDS + 5]);
    promise("同じヒープのタイルを 2 か所に結ぶと同じ中身", all[5] == all[WORDS + 5] && all[5] == cpu_word(1, 5), det);
    snprintf(det, sizeof det, "3 番の 5 語目 %08x", all[3 * WORDS + 5]);
    promise("外したタイルは 0", all[3 * WORDS + 5] == 0, det);
    snprintf(det, sizeof det, "2 番の 5 語目 %08x（%08x のはず）", all[2 * WORDS + 5], cpu_word(3, 5));
    promise("結び直すと中身がついて来る", all[2 * WORDS + 5] == cpu_word(3, 5), det);
    // 1 番は動かしていないので、そのまま見えるはず（これは約束ではなく「正しい／違う」に数える）
    expect("動かしていない 1 番の 5 語目", all[WORDS + 5], cpu_word(1, 5));
    expect("動かしていない 1 番の後半の 5 語目", all[WORDS + HALF + 5], gpu_word(1, HALF + 5));
    free(all);
}

// ---- 場面 big：仮想テクスチャ向けの大きな reserved のテクスチャ（16384x16384・RGBA8・15 段＝約 1.3 GiB）を作れるか ----
// ふつうの DirectX 12 は、作るだけではメモリを使わない。S1 は丸ごと持つので、上限（IPHONE_VK_SPARSE_MAX_MB・標準 1024 MiB）を超えて断る。
// 作れなくても「違う」には数えず「約束 …：破る」と書く。作れたら 1 タイルだけ結んで、描いた値を読み戻す
static void scene_big(void) {
    D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = 16384, .Height = 16384, .DepthOrArraySize = 1, .MipLevels = 15,
                              .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_64KB_UNDEFINED_SWIZZLE };
    HRESULT hr;
    ID3D12Resource *t = mk_reserved(&d, D3D12_RESOURCE_STATE_COPY_DEST, &hr);
    char det[96];
    snprintf(det, sizeof det, "CreateReservedResource 0x%08lx", (unsigned long) hr);
    promise("16384x16384・15 段の reserved のテクスチャを作れる", t != NULL, det);
    if (!t) return;
    UINT total = 0, nsub = 0;
    ID3D12Device_GetResourceTiling(dev, (ID3D12Resource *) t, &total, NULL, NULL, &nsub, 0, NULL);
    val("big：タイル全部 %u（%.1f MiB ぶん）", total, total * (double) TILE / 1048576.0);
    // 1 タイルだけ結び、写して読み戻す
    ID3D12Heap *h = mk_tile_heap(1, D3D12_HEAP_FLAG_ALLOW_ONLY_NON_RT_DS_TEXTURES);
    map_tile(t, 5, 7, 0, h, 0);
    D3D12_RESOURCE_DESC sd = d;
    sd.Width = 128; sd.Height = 128; sd.MipLevels = 1; sd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 tot = 0;
    ID3D12Device_GetCopyableFootprints(dev, &sd, 0, 1, 0, &fp, NULL, NULL, &tot);
    ID3D12Resource *up = mk_buf(tot, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t *p = NULL;
    D3D12_RANGE none = { 0, 0 };
    CK(ID3D12Resource_Map(up, 0, &none, (void **) &p));
    for (uint32_t y = 0; y < 128; y++)
        for (uint32_t x = 0; x < 128; x++) { uint32_t v = px_want(x, y); memcpy(p + fp.Offset + (size_t) y * fp.Footprint.RowPitch + x * 4, &v, 4); }
    ID3D12Resource_Unmap(up, 0, NULL);
    D3D12_TEXTURE_COPY_LOCATION dst = { .pResource = t, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
    D3D12_TEXTURE_COPY_LOCATION src = { .pResource = up, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = fp };
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 5 * 128, 7 * 128, 0, &src, NULL);
    ID3D12Resource *rbk = mk_buf(tot, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_BOX box = { 5 * 128, 7 * 128, 0, 6 * 128, 8 * 128, 1 };
    D3D12_TEXTURE_COPY_LOCATION rdst = { .pResource = rbk, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = fp };
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &rdst, 0, 0, 0, &dst, &box);
    run_and_wait();
    uint8_t *q = NULL;
    D3D12_RANGE all = { 0, (SIZE_T) tot };
    CK(ID3D12Resource_Map(rbk, 0, &all, (void **) &q));
    unsigned bad = 0;
    for (uint32_t y = 0; y < 128; y++)
        for (uint32_t x = 0; x < 128; x++) {
            uint32_t v;
            memcpy(&v, q + fp.Offset + (size_t) y * fp.Footprint.RowPitch + x * 4, 4);
            unsigned before = g_wrong;
            expect("big の結んだタイルの画素", v, px_want(x, y));
            bad += g_wrong != before;
        }
    ID3D12Resource_Unmap(rbk, 0, NULL);
    val("big：結んだ 1 タイルの 16384 画素を比べた・違う %u 画素", bad);
    ID3D12Resource_Release(rbk);
    ID3D12Resource_Release(up);
    ID3D12Heap_Release(h);
    ID3D12Resource_Release(t);
}

static const struct { const char *name; void (*fn)(void); } SCENES[] = {
    { "caps", scene_caps }, { "buf", scene_buf }, { "tex", scene_tex }, { "promise", scene_promise }, { "big", scene_big },
};

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    const char *want = argc > 1 ? argv[1] : "all";
    const char *e = getenv("D3D12_SPARSE_STRICT");
    g_strict = e && *e == '1';
    e = getenv("D3D12_SPARSE_BUFVIEW");
    g_bufview = e && !strcmp(e, "raw") ? 0 : e && !strcmp(e, "struct") ? 2 : 1;
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    g_compile = dc ? (PFN_COMPILE) GetProcAddress(dc, "D3DCompile") : NULL;
    if (!g_compile) { printf("d3dcompiler_47.dll が無い\n"); return 1; }
    HMODULE d12 = LoadLibraryA("d3d12.dll");
    if (!d12) { printf("失敗 d3d12.dll を読めない（%lu）\n", (unsigned long) GetLastError()); return 1; }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(d12, path, sizeof path);
    info("d3d12.dll：%s", path);
    PFN_D3D12_CREATE_DEVICE create = (PFN_D3D12_CREATE_DEVICE) (void *) GetProcAddress(d12, "D3D12CreateDevice");
    g_serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE) (void *) GetProcAddress(d12, "D3D12SerializeRootSignature");
    if (!create || !g_serialize) { printf("失敗 d3d12.dll に入口が無い\n"); return 1; }
    e = getenv("D3D12_SPARSE_DEBUG");
    if (e && *e == '1') {
        PFN_D3D12_GET_DEBUG_INTERFACE gd = (PFN_D3D12_GET_DEBUG_INTERFACE) (void *) GetProcAddress(d12, "D3D12GetDebugInterface");
        ID3D12Debug *d = NULL;
        if (gd && SUCCEEDED(gd(&IID_ID3D12Debug, (void **) &d))) { ID3D12Debug_EnableDebugLayer(d); info("調べる層を入れた"); }
        else info("調べる層が無い");
    }
    HRESULT hr = create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **) &dev);
    if (FAILED(hr)) { printf("失敗 D3D12CreateDevice（0x%08lx）\n結果 まちがいあり\n", (unsigned long) hr); return 1; }
    D3D12_COMMAND_QUEUE_DESC qd = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
    CK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **) &queue));
    CK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **) &alloc));
    CK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **) &cl));
    CK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **) &fence));
    fence_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    int ran = 0;
    unsigned total_wrong = 0;
    for (size_t i = 0; i < sizeof SCENES / sizeof *SCENES; i++) {
        if (strcmp(want, "all") && strcmp(want, SCENES[i].name)) continue;
        // promise は buf の後でだけ意味がある
        if (!strcmp(SCENES[i].name, "promise") && !g_rbuf) scene_buf();
        g_wrong = 0;
        unsigned broken0 = g_broken;
        SCENES[i].fn();
        printf("場面 %s：%s%s\n", SCENES[i].name, g_wrong ? "違う" : "正しい", g_broken > broken0 ? "（約束を破った所あり）" : "");
        fflush(stdout);
        total_wrong += g_wrong;
        ran++;
        HRESULT rr = ID3D12Device_GetDeviceRemovedReason(dev);
        if (rr != S_OK) { printf("失敗 装置が外れた 0x%08lx\n", (unsigned long) rr); total_wrong++; break; }
    }
    if (!ran) { printf("知らない場面 %s（caps・buf・tex・promise・big・all）\n", want); return 2; }
    if (total_wrong) printf("結果 %u 件違う（約束を破った所 %u）\n", total_wrong, g_broken);
    else printf("結果 全部正しい（約束を破った所 %u）\n", g_broken);
    fflush(stdout);
    return total_wrong ? 4 : 0;
}
