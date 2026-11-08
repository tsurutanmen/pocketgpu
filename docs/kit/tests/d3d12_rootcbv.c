// DirectX 12 の「壊れを見せる」試験。kit_extra\research\uab_tracking.md の U0 の 4 つの場面。
//   d3d12_rootcbv.exe [場面|all]      場面：rootcbv・rootsrv・heapafter・readback（省くと all）
//   rootcbv  ：root の CBV で、upload heap の定数を毎コマ書き換えて読む
//   rootsrv  ：root の SRV で、upload heap の buffer を毎コマ書き換えて読む
//   heapafter：heap を結んで dispatch を記録したあとに、heap の CBV の descriptor を書き換えてから submit する
//   readback ：root の UAV で、CPU から見える buffer（CUSTOM heap）に GPU が書き、CPU が直接読む
// どの場面も 1 コマごとに値を変えて submit し、fence を待つ。GPU の答えを CPU で求めた値と比べ、
// 違えば「何コマ目に・何が入っていたか（前のコマの値・次のコマの値・0・ほかの値）」を出す。
// 書き方は app\vk\tests\d3d12\d3d12test.c に合わせた。横に d3d12.dll（vkd3d-proton）を置けばそれを、無ければ Windows の DirectX 12 を使う。
// 値は「値 ...」の行に出す。最後の行は「結果 全部正しい」か「結果 n 件違う」（n は違ったコマの数の合計）。
// D3D12_ROOTCBV_FRAMES=数 でコマの数（標準 60・最大 240）。D3D12_ROOTCBV_SPIN=数 で GPU に回らせる回数（標準 20000。GPU の仕事を少し長くする）。
// D3D12_ROOTCBV_TRACK=1 で対照：同じ buffer を heap の表からも指す（rootcbv・rootsrv）・descriptor を結ぶ前に書く（heapafter）・
//   同じ buffer を heap の表の UAV からも指し、シェーダーがそこにも同じ値を書く（readback）。
//   対照で正しく、ふつうで違うなら、試験が「帳簿に載らない形」の壊れを見分けている。
// D3D12_ROOTCBV_DEBUG=1 で調べる層（ふつうの DirectX 12 だけ）
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
static unsigned g_frames = 60; // 1 場面のコマの数
static unsigned g_spin = 20000; // GPU に回らせる回数
static int g_track;            // 対照（帳簿に載る形で同じことをする）
static unsigned g_wrong;       // 違ったコマの数（場面ごと）

enum { MAX_FRAMES = 240 };

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

// ---- 仕事を送って待つ（1 コマ） ----
static void run_and_wait(void) {
    CK(ID3D12GraphicsCommandList_Close(cl));
    ID3D12CommandList *l = (ID3D12CommandList *) cl;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, &l);
    fence_val++;
    CK(ID3D12CommandQueue_Signal(queue, fence, fence_val));
    if (ID3D12Fence_GetCompletedValue(fence) < fence_val) {
        CK(ID3D12Fence_SetEventOnCompletion(fence, fence_val, fence_ev));
        if (WaitForSingleObject(fence_ev, 30000) != WAIT_OBJECT_0) die("30 秒待っても終わらない");
    }
    CK(ID3D12CommandAllocator_Reset(alloc));
    CK(ID3D12GraphicsCommandList_Reset(cl, alloc, NULL));
}

// ---- シェーダーと root signature ----
static ID3DBlob *compile(const char *src, const char *entry, const char *target) {
    ID3DBlob *code = NULL, *err = NULL;
    HRESULT hr = g_compile(src, strlen(src), "d3d12_rootcbv.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
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
static ID3D12PipelineState *mk_cs(ID3D12RootSignature *rs, const char *src) {
    ID3DBlob *cs = compile(src, "cs", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { .pRootSignature = rs, .CS = bc(cs) };
    ID3D12PipelineState *pso = NULL;
    CK(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **) &pso));
    ID3D10Blob_Release(cs);
    return pso;
}

// ---- 物を作る ----
static ID3D12Resource *mk_buf_hp(UINT64 size, const D3D12_HEAP_PROPERTIES *hp, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
                              .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = fl };
    ID3D12Resource *r = NULL;
    CK(ID3D12Device_CreateCommittedResource(dev, hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **) &r));
    return r;
}
static ID3D12Resource *mk_buf(UINT64 size, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp = { .Type = ht };
    return mk_buf_hp(size, &hp, fl, st);
}
// upload heap の buffer を作り、ずっと map したまま CPU から書ける所を返す（中身は 0）
static ID3D12Resource *mk_upload_mapped(UINT64 size, uint8_t **cpu) {
    ID3D12Resource *r = mk_buf(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p = NULL;
    D3D12_RANGE none = { 0, 0 };
    CK(ID3D12Resource_Map(r, 0, &none, &p));
    memset(p, 0, (size_t) size);
    *cpu = (uint8_t *) p;
    return r;
}
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                 .Transition = { .pResource = r, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, .StateBefore = a, .StateAfter = b } };
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &x);
}
static ID3D12DescriptorHeap *mk_heap(UINT n) {
    D3D12_DESCRIPTOR_HEAP_DESC d = { .Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, .NumDescriptors = n, .Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
    ID3D12DescriptorHeap *h = NULL;
    CK(ID3D12Device_CreateDescriptorHeap(dev, &d, &IID_ID3D12DescriptorHeap, (void **) &h));
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(ID3D12DescriptorHeap *h, UINT i) {
    D3D12_CPU_DESCRIPTOR_HANDLE c = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h);
    c.ptr += (SIZE_T) i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(ID3D12DescriptorHeap *h, UINT i) {
    D3D12_GPU_DESCRIPTOR_HANDLE g = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h);
    g.ptr += (UINT64) i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return g;
}
// 全部のバッファの SRV（raw）。対照で、同じ buffer を heap の表からも指すのに使う
static void srv_raw(ID3D12Resource *b, UINT64 size, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = { .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                           .Buffer = { .FirstElement = 0, .NumElements = (UINT) (size / 4), .Flags = D3D12_BUFFER_SRV_FLAG_RAW } };
    ID3D12Device_CreateShaderResourceView(dev, b, &sd, h);
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

// ---- コマごとの値と、違ったときの見分け ----
static uint32_t frame_value(unsigned scene, unsigned f) { return 0x01000000u * (scene + 1) + 0x1000u + f * 7u; }
static const char *what_is(unsigned scene, unsigned f, uint32_t got) {
    if (got == 0) return "0（届いていない）";
    if (f > 0 && got == frame_value(scene, f - 1)) return "前のコマの値";
    if (got == frame_value(scene, f + 1)) return "次のコマの値";
    for (unsigned k = 0; k < g_frames; k++)
        if (got == frame_value(scene, k)) return "別のコマの値";
    return "ほかの値";
}
// 1 コマを比べる。違えば数えて、はじめの 5 つだけ書く
static void check(const char *name, unsigned scene, unsigned f, uint32_t got) {
    uint32_t want = frame_value(scene, f);
    if (got == want) return;
    if (g_wrong < 5) info("%s 違う：%u コマ目 %08x（%08x のはず・%s）", name, f, got, want, what_is(scene, f, got));
    g_wrong++;
}
static void report(const char *name, unsigned first_bad) {
    if (g_wrong) val("%s %u コマ・違う %u コマ・はじめに違ったのは %u コマ目%s", name, g_frames, g_wrong, first_bad, g_track ? "（対照）" : "");
    else val("%s %u コマ・違う 0 コマ%s", name, g_frames, g_track ? "（対照）" : "");
}

// 場面 rootcbv・rootsrv・heapafter で共通のシェーダー：C の値（v）を O[f*2] に書く。O[f*2+1] は回った結果（GPU の仕事を長くするため）
static const char *CBV_HLSL =
    "cbuffer C : register(b0) { uint v; };\n"
    "cbuffer K : register(b1) { uint f; uint spin; };\n"
    "RWStructuredBuffer<uint> O : register(u0);\n"
    "[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    uint acc = v;\n"
    "    for (uint i = 0; i < spin; i++) acc = acc * 1664525u + 1013904223u;\n"
    "    O[f * 2] = v;\n"
    "    O[f * 2 + 1] = acc;\n"
    "}\n";
static const char *SRV_HLSL =
    "ByteAddressBuffer B : register(t0);\n"
    "cbuffer K : register(b1) { uint f; uint spin; };\n"
    "RWStructuredBuffer<uint> O : register(u0);\n"
    "[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    uint v = B.Load(0);\n"
    "    uint acc = v;\n"
    "    for (uint i = 0; i < spin; i++) acc = acc * 1664525u + 1013904223u;\n"
    "    O[f * 2] = v;\n"
    "    O[f * 2 + 1] = acc;\n"
    "}\n";

// 答えを入れる buffer（DEFAULT heap・UAV）。1 コマに 2 語
static ID3D12Resource *mk_out(void) {
    return mk_buf((UINT64) MAX_FRAMES * 8, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}
static void check_out(const char *name, unsigned scene, ID3D12Resource *out) {
    uint32_t *o = read_buf(out, (UINT64) MAX_FRAMES * 8, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    unsigned first_bad = 0;
    g_wrong = 0;
    for (unsigned f = 0; f < g_frames; f++) {
        unsigned before = g_wrong;
        check(name, scene, f, o[f * 2]);
        if (g_wrong == 1 && before == 0) first_bad = f;
    }
    report(name, first_bad);
    free(o);
}

// ---- 場面 rootcbv・rootsrv：root の CBV か SRV で、upload heap の 1 MB の buffer の真ん中を毎コマ書き換えて読む ----
// 真ん中に置くのは、ほかの物と同じページに乗って、たまたま一緒に受け手へ送られる道を避けるため
enum { UP_SIZE = 1 << 20, UP_OFF = 1 << 19 };
static void scene_root(int srv) {
    const char *name = srv ? "rootsrv" : "rootcbv";
    unsigned scene = srv ? 1 : 0;
    D3D12_DESCRIPTOR_RANGE tr = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND }; // 対照の表（t1・シェーダーは使わない）
    D3D12_ROOT_PARAMETER p[4] = {
        { .ParameterType = srv ? D3D12_ROOT_PARAMETER_TYPE_SRV : D3D12_ROOT_PARAMETER_TYPE_CBV, .Descriptor = { 0, 0 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV, .Descriptor = { 0, 0 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 1, 0, 2 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &tr }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_ROOT_SIGNATURE_DESC rd = { g_track ? 4 : 3, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D12RootSignature *rs = mk_rs(&rd);
    ID3D12PipelineState *pso = mk_cs(rs, srv ? SRV_HLSL : CBV_HLSL);
    uint8_t *cpu = NULL;
    ID3D12Resource *up = mk_upload_mapped(UP_SIZE, &cpu);
    ID3D12Resource *out = mk_out();
    ID3D12DescriptorHeap *h = NULL;
    if (g_track) {
        h = mk_heap(1);
        srv_raw(up, UP_SIZE, cpu_at(h, 0));
    }
    D3D12_GPU_VIRTUAL_ADDRESS va = ID3D12Resource_GetGPUVirtualAddress(up) + UP_OFF;
    for (unsigned f = 0; f < g_frames; f++) {
        uint32_t v = frame_value(scene, f);
        memcpy(cpu + UP_OFF, &v, 4); // このコマの値（前のコマの値を上書きする）
        ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs);
        ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
        if (g_track) {
            ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &h);
            ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 3, gpu_at(h, 0));
        }
        if (srv) ID3D12GraphicsCommandList_SetComputeRootShaderResourceView(cl, 0, va);
        else ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cl, 0, va);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(out));
        UINT k[2] = { f, g_spin };
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 2, 2, k, 0);
        ID3D12GraphicsCommandList_Dispatch(cl, 1, 1, 1);
        run_and_wait();
    }
    check_out(name, scene, out);
    ID3D12Resource_Unmap(up, 0, NULL);
    ID3D12Resource_Release(out);
    ID3D12Resource_Release(up);
    if (h) ID3D12DescriptorHeap_Release(h);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(rs);
}
static void scene_rootcbv(void) { scene_root(0); }
static void scene_rootsrv(void) { scene_root(1); }

// ---- 場面 heapafter：heap を結んで dispatch を記録したあとに、heap の CBV の descriptor を書き換えてから submit する ----
// CBV が指す所は、コマごとに別のページ（4 KB ずつ離す）。前のコマの descriptor が指していたページとは重ならない。
// vkd3d-proton は descriptor の表を、記録の中で dispatch のときに結ぶので、dispatch のあとに書けば「結んだあとに書いた」になる
static void scene_heapafter(void) {
    const char *name = "heapafter";
    unsigned scene = 2;
    D3D12_DESCRIPTOR_RANGE r = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER p[3] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &r }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV, .Descriptor = { 0, 0 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 1, 0, 2 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_ROOT_SIGNATURE_DESC rd = { 3, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D12RootSignature *rs = mk_rs(&rd);
    ID3D12PipelineState *pso = mk_cs(rs, CBV_HLSL);
    enum { PAGE = 4096 };
    uint8_t *cpu = NULL;
    ID3D12Resource *up = mk_upload_mapped((UINT64) MAX_FRAMES * PAGE, &cpu);
    ID3D12Resource *out = mk_out();
    ID3D12DescriptorHeap *h = mk_heap(1);
    D3D12_GPU_VIRTUAL_ADDRESS base = ID3D12Resource_GetGPUVirtualAddress(up);
    for (unsigned f = 0; f < g_frames; f++) {
        uint32_t v = frame_value(scene, f);
        memcpy(cpu + (size_t) f * PAGE, &v, 4); // このコマのページに値を書く
        D3D12_CONSTANT_BUFFER_VIEW_DESC cvd = { base + (UINT64) f * PAGE, 256 };
        if (g_track) ID3D12Device_CreateConstantBufferView(dev, &cvd, cpu_at(h, 0)); // 対照：結ぶ前に書く
        ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &h);
        ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs);
        ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
        ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 0, gpu_at(h, 0));
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(out));
        UINT k[2] = { f, g_spin };
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 2, 2, k, 0);
        ID3D12GraphicsCommandList_Dispatch(cl, 1, 1, 1);
        // ふつう：記録のあと（結んだあと）、submit の前に書く。root signature 1.0 の descriptor は volatile なので、submit の前なら書いてよい
        if (!g_track) ID3D12Device_CreateConstantBufferView(dev, &cvd, cpu_at(h, 0));
        run_and_wait();
    }
    check_out(name, scene, out);
    ID3D12Resource_Unmap(up, 0, NULL);
    ID3D12Resource_Release(out);
    ID3D12Resource_Release(up);
    ID3D12DescriptorHeap_Release(h);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(rs);
}

// ---- 場面 readback：root の UAV で、CPU から見える buffer（CUSTOM heap・WRITE_BACK）に GPU が書き、fence のあと CPU が直接読む ----
// 値は root の定数で渡す（帳簿に載る道）。読み戻しだけを試す
static const char *WB_HLSL =
    "cbuffer K : register(b0) { uint f; uint v; uint spin; };\n"
    "RWStructuredBuffer<uint> W : register(u0);\n"
    "[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    uint acc = v;\n"
    "    for (uint i = 0; i < spin; i++) acc = acc * 1664525u + 1013904223u;\n"
    "    W[f * 2] = v;\n"
    "    W[f * 2 + 1] = acc;\n"
    "}\n";
// 対照：同じ buffer を heap の表の UAV（u1）からも指し、そこにも同じ値を書く。
// ドライバは、表で指しただけの UAV は読み戻さない（結んだ pipeline のシェーダーが、その場所に書くときだけ「書く範囲」にする。icd.c の wr_may_cb）。
// だから対照では、シェーダーが表の UAV にも実際に書く
static const char *WB_TRACK_HLSL =
    "cbuffer K : register(b0) { uint f; uint v; uint spin; };\n"
    "RWStructuredBuffer<uint> W : register(u0);\n"
    "RWStructuredBuffer<uint> T : register(u1);\n"
    "[numthreads(1, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    uint acc = v;\n"
    "    for (uint i = 0; i < spin; i++) acc = acc * 1664525u + 1013904223u;\n"
    "    W[f * 2] = v;\n"
    "    W[f * 2 + 1] = acc;\n"
    "    T[f * 2] = v;\n"
    "    T[f * 2 + 1] = acc;\n"
    "}\n";
static void scene_readback(void) {
    const char *name = "readback";
    unsigned scene = 3;
    D3D12_DESCRIPTOR_RANGE tr = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND }; // 対照の表（u1）
    D3D12_ROOT_PARAMETER p[3] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 0, 0, 3 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV, .Descriptor = { 0, 0 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &tr }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_ROOT_SIGNATURE_DESC rd = { g_track ? 3 : 2, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D12RootSignature *rs = mk_rs(&rd);
    ID3D12PipelineState *pso = mk_cs(rs, g_track ? WB_TRACK_HLSL : WB_HLSL);
    D3D12_HEAP_PROPERTIES hp = { .Type = D3D12_HEAP_TYPE_CUSTOM, .CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
                                 .MemoryPoolPreference = D3D12_MEMORY_POOL_L0 };
    UINT64 size = (UINT64) MAX_FRAMES * 8;
    ID3D12Resource *w = mk_buf_hp(size, &hp, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    void *mp = NULL;
    D3D12_RANGE all = { 0, (SIZE_T) size };
    CK(ID3D12Resource_Map(w, 0, &all, &mp));
    memset(mp, 0, (size_t) size);
    const volatile uint32_t *wp = (const volatile uint32_t *) mp;
    barrier(w, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12DescriptorHeap *h = NULL;
    if (g_track) {
        // 対照：同じ buffer 全体の UAV を、結ぶ前に heap に書いておく
        h = mk_heap(1);
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = { .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
                                                .Buffer = { .FirstElement = 0, .NumElements = (UINT) (size / 4), .StructureByteStride = 4 } };
        ID3D12Device_CreateUnorderedAccessView(dev, w, NULL, &ud, cpu_at(h, 0));
    }
    unsigned first_bad = 0;
    g_wrong = 0;
    for (unsigned f = 0; f < g_frames; f++) {
        ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs);
        ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
        if (g_track) {
            ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &h);
            ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 2, gpu_at(h, 0));
        }
        UINT k[3] = { f, frame_value(scene, f), g_spin };
        ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, 3, k, 0);
        ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 1, ID3D12Resource_GetGPUVirtualAddress(w));
        ID3D12GraphicsCommandList_Dispatch(cl, 1, 1, 1);
        run_and_wait();
        unsigned before = g_wrong;
        check(name, scene, f, wp[f * 2]); // fence を待ったので、GPU の書いた値が見えるはず
        if (g_wrong == 1 && before == 0) first_bad = f;
    }
    report(name, first_bad);
    ID3D12Resource_Unmap(w, 0, NULL);
    ID3D12Resource_Release(w);
    if (h) ID3D12DescriptorHeap_Release(h);
    ID3D12PipelineState_Release(pso);
    ID3D12RootSignature_Release(rs);
}

static const struct { const char *name; void (*fn)(void); } SCENES[] = {
    { "rootcbv", scene_rootcbv }, { "rootsrv", scene_rootsrv }, { "heapafter", scene_heapafter }, { "readback", scene_readback },
};

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    const char *want = argc > 1 ? argv[1] : "all";
    const char *e = getenv("D3D12_ROOTCBV_FRAMES");
    if (e && atoi(e) > 0) g_frames = (unsigned) atoi(e) > MAX_FRAMES ? MAX_FRAMES : (unsigned) atoi(e);
    e = getenv("D3D12_ROOTCBV_SPIN");
    if (e) g_spin = (unsigned) strtoul(e, NULL, 0);
    e = getenv("D3D12_ROOTCBV_TRACK");
    g_track = e && *e == '1';
    HMODULE dc = LoadLibraryA("d3dcompiler_47.dll");
    g_compile = dc ? (PFN_COMPILE) GetProcAddress(dc, "D3DCompile") : NULL;
    if (!g_compile) { printf("d3dcompiler_47.dll が無い\n"); return 1; }
    HMODULE d12 = LoadLibraryA("d3d12.dll");
    if (!d12) { printf("失敗 d3d12.dll を読めない（%lu）\n", (unsigned long) GetLastError()); return 1; }
    char path[MAX_PATH] = "?";
    GetModuleFileNameA(d12, path, sizeof path);
    info("d3d12.dll：%s", path);
    info("コマ %u・回る回数 %u%s", g_frames, g_spin, g_track ? "・対照" : "");
    PFN_D3D12_CREATE_DEVICE create = (PFN_D3D12_CREATE_DEVICE) (void *) GetProcAddress(d12, "D3D12CreateDevice");
    g_serialize = (PFN_D3D12_SERIALIZE_ROOT_SIGNATURE) (void *) GetProcAddress(d12, "D3D12SerializeRootSignature");
    if (!create || !g_serialize) { printf("失敗 d3d12.dll に入口が無い\n"); return 1; }
    e = getenv("D3D12_ROOTCBV_DEBUG");
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
        g_wrong = 0;
        LARGE_INTEGER fq, t0, t1;
        QueryPerformanceFrequency(&fq);
        QueryPerformanceCounter(&t0);
        SCENES[i].fn();
        QueryPerformanceCounter(&t1);
        info("%s は %.1f ms", SCENES[i].name, (double) (t1.QuadPart - t0.QuadPart) * 1000.0 / (double) fq.QuadPart);
        printf("場面 %s：%s\n", SCENES[i].name, g_wrong ? "違う" : "正しい");
        total_wrong += g_wrong;
        ran++;
    }
    if (!ran) { printf("知らない場面 %s（rootcbv・rootsrv・heapafter・readback・all）\n", want); return 2; }
    HRESULT rr = ID3D12Device_GetDeviceRemovedReason(dev);
    if (rr != S_OK) { printf("失敗 装置が外れた 0x%08lx\n", (unsigned long) rr); total_wrong++; }
    if (total_wrong) printf("結果 %u 件違う\n", total_wrong);
    else printf("結果 全部正しい\n");
    fflush(stdout);
    return total_wrong ? 4 : 0;
}
