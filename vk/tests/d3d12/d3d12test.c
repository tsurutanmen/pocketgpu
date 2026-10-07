// DirectX 12 の小さな試験。窓を作らず、画面の外の的に描いて読み出し、CPU で出した答えと比べる。
//   d3d12test.exe [場面|all] [絵の頭]      場面：tri・add・bind・tex・view（省くと all）
//   tri ：頂点バッファで三角形を描く（RTV・クリア・読み出し）
//   add ：compute shader で足し算（root の定数・SRV の表・root の UAV）
//   bind：compute shader で CBV・SRV・UAV・sampler を使う（root の CBV・表の CBV/SRV/UAV・sampler の表・static sampler）
//   tex ：画素 shader でテクスチャを読む（root の CBV・SRV の表・static sampler・root の定数）
//   view：buffer の途中から始まる見え方（structured・raw・typed の SRV と typed の UAV）
// 横に d3d12.dll（vkd3d-proton）を置けばそれを、無ければ Windows の DirectX 12 を使う。
// 値は「値 ...」の行に出す（作りどうしで比べる行）。最後の行は「結果 全部正しい」か「結果 まちがいあり」
// D3D12TEST_HEAP=数 でシェーダーから見える heap をその大きさで作り、作る時間を出す。D3D12TEST_DEBUG=1 で調べる層（ふつうの DirectX 12 だけ）
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#define PSAPI_VERSION 2
#include <psapi.h>
#include <math.h>
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
static const char *g_ppm; // 絵の頭（無ければ書かない）
static int g_bad;         // まちがいの数（場面ごと）

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

// ---- 仕事を送って待つ ----
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
    HRESULT hr = g_compile(src, strlen(src), "d3d12test.hlsl", NULL, NULL, entry, target, 0, 0, &code, &err);
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

// ---- 物を作る ----
static ID3D12Resource *mk_buf(UINT64 size, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp = { .Type = ht };
    D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_BUFFER, .Width = size, .Height = 1, .DepthOrArraySize = 1, .MipLevels = 1,
                              .Format = DXGI_FORMAT_UNKNOWN, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR, .Flags = fl };
    ID3D12Resource *r = NULL;
    CK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **) &r));
    return r;
}
static ID3D12Resource *mk_upload(const void *data, UINT64 size) {
    ID3D12Resource *r = mk_buf(size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_GENERIC_READ);
    void *p = NULL;
    D3D12_RANGE none = { 0, 0 };
    CK(ID3D12Resource_Map(r, 0, &none, &p));
    memcpy(p, data, (size_t) size);
    ID3D12Resource_Unmap(r, 0, NULL);
    return r;
}
static ID3D12Resource *mk_tex(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE *cv) {
    D3D12_HEAP_PROPERTIES hp = { .Type = D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC d = { .Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D, .Width = w, .Height = h, .DepthOrArraySize = 1, .MipLevels = 1,
                              .Format = f, .SampleDesc = { 1, 0 }, .Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN, .Flags = fl };
    ID3D12Resource *r = NULL;
    CK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, cv, &IID_ID3D12Resource, (void **) &r));
    return r;
}
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x = { .Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
                                 .Transition = { .pResource = r, .Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, .StateBefore = a, .StateAfter = b } };
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &x);
}
// D3D12TEST_HEAP=数 で、シェーダーから見える heap をその大きさで作る（ゲームは 100 万の heap を 1 つ作ることが多い。作る時間を測る）
static UINT g_heap_min;
static ID3D12DescriptorHeap *mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE t, UINT n, int visible) {
    if (visible && t == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV && n < g_heap_min) n = g_heap_min;
    if (visible && t == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER && n < g_heap_min) n = g_heap_min < 2048 ? g_heap_min : 2048;
    D3D12_DESCRIPTOR_HEAP_DESC d = { .Type = t, .NumDescriptors = n, .Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
    ID3D12DescriptorHeap *h = NULL;
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    CK(ID3D12Device_CreateDescriptorHeap(dev, &d, &IID_ID3D12DescriptorHeap, (void **) &h));
    QueryPerformanceCounter(&t1);
    if (g_heap_min) info("heap（種類 %d・%u 個）を作るのに %.1f ms", (int) t, n, (double) (t1.QuadPart - t0.QuadPart) * 1000.0 / (double) f.QuadPart);
    return h;
}
static D3D12_CPU_DESCRIPTOR_HANDLE cpu_at(ID3D12DescriptorHeap *h, UINT i) {
    D3D12_DESCRIPTOR_HEAP_DESC d = ID3D12DescriptorHeap_GetDesc(h);
    D3D12_CPU_DESCRIPTOR_HANDLE c = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(h);
    c.ptr += (SIZE_T) i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, d.Type);
    return c;
}
static D3D12_GPU_DESCRIPTOR_HANDLE gpu_at(ID3D12DescriptorHeap *h, UINT i) {
    D3D12_DESCRIPTOR_HEAP_DESC d = ID3D12DescriptorHeap_GetDesc(h);
    D3D12_GPU_DESCRIPTOR_HANDLE g = ID3D12DescriptorHeap_GetGPUDescriptorHandleForHeapStart(h);
    g.ptr += (UINT64) i * ID3D12Device_GetDescriptorHandleIncrementSize(dev, d.Type);
    return g;
}

// 画像を読み出し用のバッファに写して、CPU から見える所を返す（行の長さは *pitch）
static ID3D12Resource *g_rb;
static const uint8_t *read_tex(ID3D12Resource *t, D3D12_RESOURCE_STATES st, UINT *pitch) {
    D3D12_RESOURCE_DESC d = ID3D12Resource_GetDesc(t);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total = 0;
    ID3D12Device_GetCopyableFootprints(dev, &d, 0, 1, 0, &fp, NULL, NULL, &total);
    g_rb = mk_buf(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(t, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dst = { .pResource = g_rb, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = fp };
    D3D12_TEXTURE_COPY_LOCATION src = { .pResource = t, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    run_and_wait();
    void *p = NULL;
    D3D12_RANGE all = { 0, (SIZE_T) total };
    CK(ID3D12Resource_Map(g_rb, 0, &all, &p));
    *pitch = fp.Footprint.RowPitch;
    return (const uint8_t *) p + fp.Offset;
}
static const void *read_buf(ID3D12Resource *b, UINT64 size, D3D12_RESOURCE_STATES st) {
    g_rb = mk_buf(size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    barrier(b, st, D3D12_RESOURCE_STATE_COPY_SOURCE);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, g_rb, 0, b, 0, size);
    run_and_wait();
    void *p = NULL;
    D3D12_RANGE all = { 0, (SIZE_T) size };
    CK(ID3D12Resource_Map(g_rb, 0, &all, &p));
    return p;
}
static void done_read(void) {
    ID3D12Resource_Unmap(g_rb, 0, NULL);
    ID3D12Resource_Release(g_rb);
    g_rb = NULL;
}

// RGBA8 の画素を PPM に書き、全部の画素の検算（FNV）を返す
static uint32_t save_rgba8(const char *scene, const uint8_t *p, UINT pitch, int w, int h) {
    uint32_t sum = 2166136261u;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w * 4; x++) sum = (sum ^ p[(size_t) y * pitch + x]) * 16777619u;
    if (g_ppm) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s%s.ppm", g_ppm, scene);
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) fwrite(p + (size_t) y * pitch + x * 4, 1, 3, f);
            fclose(f);
        }
    }
    return sum;
}

// 4x4 の RGBA8 のテクスチャ（場面 bind と tex で使う）
static uint8_t g_texel[4][4][4];
static ID3D12Resource *mk_tex4(ID3D12Resource **up_keep) {
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            g_texel[y][x][0] = (uint8_t) (x * 64 + 32);
            g_texel[y][x][1] = (uint8_t) (y * 64 + 32);
            g_texel[y][x][2] = ((x + y) & 1) ? 255 : 0;
            g_texel[y][x][3] = (uint8_t) (255 - x * 16 - y * 4);
        }
    ID3D12Resource *t = mk_tex(4, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, NULL);
    D3D12_RESOURCE_DESC d = ID3D12Resource_GetDesc(t);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT64 total = 0;
    ID3D12Device_GetCopyableFootprints(dev, &d, 0, 1, 0, &fp, NULL, NULL, &total);
    uint8_t *stage = calloc(1, (size_t) total);
    for (int y = 0; y < 4; y++) memcpy(stage + fp.Offset + (size_t) y * fp.Footprint.RowPitch, g_texel[y], 16);
    ID3D12Resource *up = mk_upload(stage, total);
    free(stage);
    D3D12_TEXTURE_COPY_LOCATION dst = { .pResource = t, .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = 0 };
    D3D12_TEXTURE_COPY_LOCATION src = { .pResource = up, .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = fp };
    ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, NULL);
    barrier(t, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    *up_keep = up;
    return t;
}
static void srv_tex4(ID3D12Resource *t, D3D12_CPU_DESCRIPTOR_HANDLE h) {
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = { .MipLevels = 1 } };
    ID3D12Device_CreateShaderResourceView(dev, t, &sd, h);
}
static float texf(int x, int y, int c) {
    x = x < 0 ? 0 : x > 3 ? 3 : x;
    y = y < 0 ? 0 : y > 3 ? 3 : y;
    return g_texel[y][x][c] / 255.0f;
}

static D3D12_GRAPHICS_PIPELINE_STATE_DESC gfx_desc(ID3D12RootSignature *rs, ID3DBlob *vs, ID3DBlob *ps, DXGI_FORMAT rt) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = { 0 };
    g.pRootSignature = rs;
    g.VS = bc(vs);
    g.PS = bc(ps);
    g.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    g.SampleMask = UINT_MAX;
    g.RasterizerState = (D3D12_RASTERIZER_DESC) { .FillMode = D3D12_FILL_MODE_SOLID, .CullMode = D3D12_CULL_MODE_NONE, .DepthClipEnable = TRUE };
    g.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    g.NumRenderTargets = 1;
    g.RTVFormats[0] = rt;
    g.SampleDesc.Count = 1;
    return g;
}

// ---- 場面 tri：頂点バッファの三角形 ----
static const char *TRI_HLSL =
    "struct V { float2 p : POS; float3 c : COL; };\n"
    "void vs(V v, out float4 o : SV_Position, out float3 c : COL) { o = float4(v.p, 0, 1); c = v.c; }\n"
    "float4 ps(float4 p : SV_Position, float3 c : COL) : SV_Target { return float4(c, 1); }\n";

static void scene_tri(void) {
    enum { W = 256, H = 256 };
    static const float vtx[3][5] = { { -0.8f, -0.8f, 1, 0, 0 }, { 0.8f, -0.8f, 0, 1, 0 }, { 0.0f, 0.8f, 0, 0, 1 } };
    static const float clear[4] = { 0.1f, 0.2f, 0.3f, 1.0f };
    D3D12_ROOT_SIGNATURE_DESC rd = { .Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT };
    ID3D12RootSignature *rs = mk_rs(&rd);
    ID3DBlob *vs = compile(TRI_HLSL, "vs", "vs_5_0"), *ps = compile(TRI_HLSL, "ps", "ps_5_0");
    D3D12_INPUT_ELEMENT_DESC ie[] = {
        { "POS", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = gfx_desc(rs, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM);
    g.InputLayout = (D3D12_INPUT_LAYOUT_DESC) { ie, 2 };
    ID3D12PipelineState *pso = NULL;
    CK(ID3D12Device_CreateGraphicsPipelineState(dev, &g, &IID_ID3D12PipelineState, (void **) &pso));
    ID3D12Resource *vb = mk_upload(vtx, sizeof vtx);
    D3D12_CLEAR_VALUE cv = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .Color = { clear[0], clear[1], clear[2], clear[3] } };
    ID3D12Resource *rt = mk_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, &cv);
    ID3D12DescriptorHeap *rtvh = mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_at(rtvh, 0);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);

    ID3D12GraphicsCommandList_ClearRenderTargetView(cl, rtv, clear, 0, NULL);
    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D12_VERTEX_BUFFER_VIEW vbv = { ID3D12Resource_GetGPUVirtualAddress(vb), sizeof vtx, sizeof vtx[0] };
    ID3D12GraphicsCommandList_IASetVertexBuffers(cl, 0, 1, &vbv);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    UINT pitch = 0;
    const uint8_t *px = read_tex(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, &pitch);

    // CPU で同じ三角形を描いて比べる（画素の中心で辺の内側か。辺の上すれすれの画素は数えない）
    float vx[3], vy[3];
    for (int i = 0; i < 3; i++) { vx[i] = (vtx[i][0] + 1) * 0.5f * W; vy[i] = (1 - vtx[i][1]) * 0.5f * H; }
    float area = (vx[1] - vx[0]) * (vy[2] - vy[0]) - (vy[1] - vy[0]) * (vx[2] - vx[0]);
    unsigned colored = 0, wrong = 0, edge = 0;
    uint8_t cc[3];
    for (int k = 0; k < 3; k++) cc[k] = (uint8_t) lrintf(clear[k] * 255);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *p = px + (size_t) y * pitch + x * 4;
            float cx = x + 0.5f, cy = y + 0.5f, w[3];
            for (int i = 0; i < 3; i++) {
                int a = (i + 1) % 3, b = (i + 2) % 3;
                w[i] = ((vx[b] - vx[a]) * (cy - vy[a]) - (vy[b] - vy[a]) * (cx - vx[a])) / area;
            }
            int in = w[0] >= 0 && w[1] >= 0 && w[2] >= 0;
            float closest = fminf(fabsf(w[0]), fminf(fabsf(w[1]), fabsf(w[2])));
            if (abs(p[0] - cc[0]) > 2 || abs(p[1] - cc[1]) > 2 || abs(p[2] - cc[2]) > 2) colored++;
            if (closest < 0.004f) { edge++; continue; }
            int ok = 1;
            for (int k = 0; k < 3; k++) {
                int want = in ? (int) lrintf(w[0] * vtx[0][2 + k] * 255 + w[1] * vtx[1][2 + k] * 255 + w[2] * vtx[2][2 + k] * 255) : cc[k];
                if (abs(p[k] - want) > 2) ok = 0;
            }
            if (!ok) wrong++;
        }
    const uint8_t *m = px + (size_t) (H / 2) * pitch + (W / 2) * 4;
    uint32_t sum = save_rgba8("tri", px, pitch, W, H);
    val("tri 色のついた画素 %u・まん中 %u %u %u・すみ %u %u %u・検算 %08x", colored, m[0], m[1], m[2], px[0], px[1], px[2], sum);
    info("tri CPU の絵と違う画素 %u（辺の上すれすれで数えない画素 %u）", wrong, edge);
    if (wrong || colored < 20000 || colored > 22000) g_bad++;
    done_read();
}

// ---- 場面 add：compute shader の足し算 ----
static const char *ADD_HLSL =
    "StructuredBuffer<uint> A : register(t0);\n"
    "ByteAddressBuffer B : register(t1);\n"
    "RWStructuredBuffer<uint> C : register(u0);\n"
    "cbuffer K : register(b0) { uint n; uint mul; };\n"
    "[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x < n) C[id.x] = A[id.x] * mul + B.Load(id.x * 4);\n"
    "}\n";

static void scene_add(void) {
    enum { N = 65536 };
    D3D12_DESCRIPTOR_RANGE r = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER p[3] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 0, 0, 2 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &r }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV, .Descriptor = { 0, 0 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_ROOT_SIGNATURE_DESC rd = { 3, p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D12RootSignature *rs = mk_rs(&rd);
    ID3DBlob *cs = compile(ADD_HLSL, "cs", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { .pRootSignature = rs, .CS = bc(cs) };
    ID3D12PipelineState *pso = NULL;
    CK(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **) &pso));

    uint32_t *a = malloc(N * 4), *b = malloc(N * 4);
    for (uint32_t i = 0; i < N; i++) { a[i] = i; b[i] = 3 * i + 7; }
    ID3D12Resource *ua = mk_upload(a, N * 4), *ub = mk_upload(b, N * 4);
    ID3D12Resource *ga = mk_buf(N * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *gb = mk_buf(N * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *gc = mk_buf(N * 4, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, ga, 0, ua, 0, N * 4);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, gb, 0, ub, 0, N * 4);
    barrier(ga, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(gb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ID3D12DescriptorHeap *h = mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sa = { .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                           .Buffer = { .FirstElement = 0, .NumElements = N, .StructureByteStride = 4 } };
    D3D12_SHADER_RESOURCE_VIEW_DESC sb = { .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                           .Buffer = { .FirstElement = 0, .NumElements = N, .Flags = D3D12_BUFFER_SRV_FLAG_RAW } };
    ID3D12Device_CreateShaderResourceView(dev, ga, &sa, cpu_at(h, 0));
    ID3D12Device_CreateShaderResourceView(dev, gb, &sb, cpu_at(h, 1));

    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &h);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    UINT k[2] = { N, 5 };
    ID3D12GraphicsCommandList_SetComputeRoot32BitConstants(cl, 0, 2, k, 0);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 1, gpu_at(h, 0));
    ID3D12GraphicsCommandList_SetComputeRootUnorderedAccessView(cl, 2, ID3D12Resource_GetGPUVirtualAddress(gc));
    ID3D12GraphicsCommandList_Dispatch(cl, N / 64, 1, 1);
    const uint32_t *c = read_buf(gc, N * 4, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    unsigned wrong = 0;
    uint32_t sum = 2166136261u;
    for (uint32_t i = 0; i < N; i++) {
        uint32_t want = a[i] * 5 + b[i];
        if (c[i] != want) { if (wrong < 3) info("add 違う c[%u] = %u（%u のはず）", i, c[i], want); wrong++; }
        sum = (sum ^ c[i]) * 16777619u;
    }
    val("add %u 個・違う %u 個・c[1] %u・c[%u] %u・検算 %08x", N, wrong, c[1], N - 1, c[N - 1], sum);
    if (wrong) g_bad++;
    done_read();
    free(a);
    free(b);
}

// ---- 場面 bind：CBV・SRV・UAV・sampler ----
static const char *BIND_HLSL =
    "cbuffer P : register(b0) { float4 scale; float4 bias; };\n"
    "cbuffer Q : register(b1) { float4 add; };\n"
    "Texture2D<float4> T : register(t0);\n"
    "SamplerState S0 : register(s0);\n" // 表の sampler（点）
    "SamplerState S1 : register(s1);\n" // static sampler（線形）
    "RWTexture2D<float4> O : register(u0);\n"
    "[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    float2 uv = (id.xy + 0.5) / 8.0;\n"
    "    O[id.xy] = T.SampleLevel(S0, uv, 0) * scale + T.SampleLevel(S1, uv, 0) * bias + add;\n"
    "}\n";

static void scene_bind(void) {
    D3D12_DESCRIPTOR_RANGE r[3] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
    };
    D3D12_DESCRIPTOR_RANGE rs_ = { D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 1, 0, 0, 0 };
    D3D12_ROOT_PARAMETER p[3] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV, .Descriptor = { 0, 0 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 3, r }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &rs_ }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL },
    };
    D3D12_STATIC_SAMPLER_DESC ss = { .Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR, .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                     .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                                     .MaxLOD = D3D12_FLOAT32_MAX, .ShaderRegister = 1, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL };
    D3D12_ROOT_SIGNATURE_DESC rd = { 3, p, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D12RootSignature *rs = mk_rs(&rd);
    ID3DBlob *cs = compile(BIND_HLSL, "cs", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { .pRootSignature = rs, .CS = bc(cs) };
    ID3D12PipelineState *pso = NULL;
    CK(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **) &pso));

    ID3D12Resource *up = NULL, *tex = mk_tex4(&up);
    ID3D12Resource *out = mk_tex(8, 8, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, NULL);
    static const float scale[4] = { 0.5f, 1.0f, 0.25f, 1.0f }, bias[4] = { 0.5f, 0.25f, 1.0f, 0.0f }, add[4] = { 0.01f, 0.02f, 0.03f, 0.04f };
    float cbd[128] = { 0 }; // b0 は 0 バイト目・b1 は 256 バイト目
    memcpy(cbd, scale, 16);
    memcpy(cbd + 4, bias, 16);
    memcpy(cbd + 64, add, 16);
    ID3D12Resource *cb = mk_upload(cbd, sizeof cbd);

    ID3D12DescriptorHeap *h = mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, 1), *sh = mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 1, 1);
    D3D12_CONSTANT_BUFFER_VIEW_DESC cvd = { ID3D12Resource_GetGPUVirtualAddress(cb) + 256, 256 };
    ID3D12Device_CreateConstantBufferView(dev, &cvd, cpu_at(h, 0));
    srv_tex4(tex, cpu_at(h, 1));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uvd = { .Format = DXGI_FORMAT_R32G32B32A32_FLOAT, .ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D };
    ID3D12Device_CreateUnorderedAccessView(dev, out, NULL, &uvd, cpu_at(h, 2));
    D3D12_SAMPLER_DESC smp = { .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT, .AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP,
                               .AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, .AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP, .MaxLOD = D3D12_FLOAT32_MAX };
    ID3D12Device_CreateSampler(dev, &smp, cpu_at(sh, 0));

    ID3D12DescriptorHeap *hs[2] = { h, sh };
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 2, hs);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    ID3D12GraphicsCommandList_SetComputeRootConstantBufferView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 1, gpu_at(h, 0));
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 2, gpu_at(sh, 0));
    ID3D12GraphicsCommandList_Dispatch(cl, 1, 1, 1);
    UINT pitch = 0;
    const uint8_t *px = read_tex(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &pitch);
    unsigned wrong = 0;
    float worst = 0;
    uint32_t sum = 2166136261u;
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            const float *o = (const float *) (px + (size_t) y * pitch + x * 16);
            // 線形：テクセルの座標は (x + 0.5) / 2 - 0.5。端は clamp
            float tx = (x + 0.5f) / 2 - 0.5f, ty = (y + 0.5f) / 2 - 0.5f;
            int x0 = (int) floorf(tx), y0 = (int) floorf(ty);
            float fx = tx - x0, fy = ty - y0;
            int bad = 0;
            for (int c = 0; c < 4; c++) {
                float pt = texf(x / 2, y / 2, c);
                float ln = (1 - fx) * (1 - fy) * texf(x0, y0, c) + fx * (1 - fy) * texf(x0 + 1, y0, c) + (1 - fx) * fy * texf(x0, y0 + 1, c) + fx * fy * texf(x0 + 1, y0 + 1, c);
                float want = pt * scale[c] + ln * bias[c] + add[c];
                float d = fabsf(o[c] - want);
                if (d > worst) worst = d;
                if (d > 0.01f) bad = 1;
                sum = (sum ^ (uint32_t) lrintf(o[c] * 1000)) * 16777619u;
            }
            if (bad) { if (wrong < 3) info("bind 違う (%d,%d) = %.3f %.3f %.3f %.3f", x, y, o[0], o[1], o[2], o[3]); wrong++; }
        }
    const float *o0 = (const float *) px, *o7 = (const float *) (px + 7 * (size_t) pitch + 7 * 16);
    val("bind 64 画素・違う %u・(0,0) %.3f %.3f %.3f %.3f・(7,7) %.3f %.3f %.3f %.3f・検算 %08x", wrong, o0[0], o0[1], o0[2], o0[3], o7[0], o7[1], o7[2], o7[3], sum);
    info("bind CPU の答えとのいちばん大きい差 %.5f", worst);
    if (wrong) g_bad++;
    done_read();
}

// ---- 場面 tex：画素 shader でテクスチャを読む ----
static const char *TEX_HLSL =
    "cbuffer P : register(b0) { float4 tint; };\n"
    "cbuffer K : register(b1) { float4 plus; };\n"
    "Texture2D<float4> T : register(t0);\n"
    "SamplerState S : register(s0);\n"
    "float4 vs(uint id : SV_VertexID) : SV_Position { float2 p = float2((id << 1) & 2, id & 2); return float4(p * 2 - 1, 0, 1); }\n"
    "float4 ps(float4 pos : SV_Position) : SV_Target { return saturate(T.Sample(S, pos.xy / 64.0) * tint + plus); }\n";

static void scene_tex(void) {
    enum { W = 64, H = 64 };
    D3D12_DESCRIPTOR_RANGE r = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_ROOT_PARAMETER p[3] = {
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV, .Descriptor = { 0, 0 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 1, &r }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL },
        { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS, .Constants = { 1, 0, 4 }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL },
    };
    D3D12_STATIC_SAMPLER_DESC ss = { .Filter = D3D12_FILTER_MIN_MAG_MIP_POINT, .AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                     .AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP, .AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                     .MaxLOD = D3D12_FLOAT32_MAX, .ShaderRegister = 0, .ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL };
    D3D12_ROOT_SIGNATURE_DESC rd = { 3, p, 1, &ss, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D12RootSignature *rs = mk_rs(&rd);
    ID3DBlob *vs = compile(TEX_HLSL, "vs", "vs_5_0"), *ps = compile(TEX_HLSL, "ps", "ps_5_0");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC g = gfx_desc(rs, vs, ps, DXGI_FORMAT_R8G8B8A8_UNORM);
    ID3D12PipelineState *pso = NULL;
    CK(ID3D12Device_CreateGraphicsPipelineState(dev, &g, &IID_ID3D12PipelineState, (void **) &pso));

    ID3D12Resource *up = NULL, *tex = mk_tex4(&up);
    static const float tint[64] = { 1.0f, 0.5f, 0.25f, 1.0f }, plus[4] = { 0.0f, 0.1f, 0.2f, 0.0f };
    ID3D12Resource *cb = mk_upload(tint, sizeof tint);
    ID3D12Resource *rt = mk_tex(W, H, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, NULL);
    ID3D12DescriptorHeap *rtvh = mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, 0), *h = mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, 1);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = cpu_at(rtvh, 0);
    ID3D12Device_CreateRenderTargetView(dev, rt, NULL, rtv);
    srv_tex4(tex, cpu_at(h, 0));

    ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &h);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(cl, 0, ID3D12Resource_GetGPUVirtualAddress(cb));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(cl, 1, gpu_at(h, 0));
    ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 2, 4, plus, 0);
    D3D12_VIEWPORT vp = { 0, 0, W, H, 0, 1 };
    D3D12_RECT sc = { 0, 0, W, H };
    ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
    UINT pitch = 0;
    const uint8_t *px = read_tex(rt, D3D12_RESOURCE_STATE_RENDER_TARGET, &pitch);
    unsigned wrong = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const uint8_t *q = px + (size_t) y * pitch + x * 4;
            int bad = 0;
            for (int c = 0; c < 4; c++) {
                float v = texf(x / 16, y / 16, c) * tint[c] + plus[c];
                v = v < 0 ? 0 : v > 1 ? 1 : v;
                if (abs(q[c] - (int) lrintf(v * 255)) > 1) bad = 1;
            }
            if (bad) { if (wrong < 3) info("tex 違う (%d,%d) = %u %u %u %u", x, y, q[0], q[1], q[2], q[3]); wrong++; }
        }
    uint32_t sum = save_rgba8("tex", px, pitch, W, H);
    const uint8_t *m = px + (size_t) 40 * pitch + 20 * 4;
    val("tex 4096 画素・違う %u・(20,40) %u %u %u %u・検算 %08x", wrong, m[0], m[1], m[2], m[3], sum);
    if (wrong) g_bad++;
    done_read();
}

// ---- 場面 view：buffer の途中から始まる見え方（structured・raw・typed の SRV と typed の UAV） ----
// iPhone の GPU は texel buffer の始まりを 64 バイトに揃える必要がある。始まりが 12・16・20・28 バイトの見え方で正しく読み書きできるか
static const char *VIEW_HLSL =
    "StructuredBuffer<uint> A : register(t0);\n"
    "ByteAddressBuffer B : register(t1);\n"
    "Buffer<uint> T : register(t2);\n"
    "RWBuffer<uint> O : register(u0);\n"
    "[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "    if (id.x < 256) O[id.x] = A[id.x] + B.Load(id.x * 4) * 10 + T[id.x] * 100;\n"
    "}\n";

static void scene_view(void) {
    enum { NS = 1024, NO = 512 };
    D3D12_DESCRIPTOR_RANGE r[2] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND },
    };
    D3D12_ROOT_PARAMETER p = { .ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE, .DescriptorTable = { 2, r }, .ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL };
    D3D12_ROOT_SIGNATURE_DESC rd = { 1, &p, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE };
    ID3D12RootSignature *rs = mk_rs(&rd);
    ID3DBlob *cs = compile(VIEW_HLSL, "cs", "cs_5_0");
    D3D12_COMPUTE_PIPELINE_STATE_DESC cd = { .pRootSignature = rs, .CS = bc(cs) };
    ID3D12PipelineState *pso = NULL;
    CK(ID3D12Device_CreateComputePipelineState(dev, &cd, &IID_ID3D12PipelineState, (void **) &pso));

    uint32_t src[NS], zero[NO];
    for (uint32_t i = 0; i < NS; i++) src[i] = i;
    memset(zero, 0, sizeof zero);
    ID3D12Resource *us = mk_upload(src, sizeof src), *uz = mk_upload(zero, sizeof zero);
    ID3D12Resource *gs = mk_buf(sizeof src, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12Resource *go = mk_buf(sizeof zero, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, gs, 0, us, 0, sizeof src);
    ID3D12GraphicsCommandList_CopyBufferRegion(cl, go, 0, uz, 0, sizeof zero);
    barrier(gs, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    barrier(go, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ID3D12DescriptorHeap *h = mk_heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 4, 1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sa = { .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                           .Buffer = { .FirstElement = 3, .NumElements = 256, .StructureByteStride = 4 } };
    D3D12_SHADER_RESOURCE_VIEW_DESC sb = { .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                           .Buffer = { .FirstElement = 4, .NumElements = 256, .Flags = D3D12_BUFFER_SRV_FLAG_RAW } };
    D3D12_SHADER_RESOURCE_VIEW_DESC st = { .Format = DXGI_FORMAT_R32_UINT, .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
                                           .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
                                           .Buffer = { .FirstElement = 5, .NumElements = 256 } };
    D3D12_UNORDERED_ACCESS_VIEW_DESC uo = { .Format = DXGI_FORMAT_R32_UINT, .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
                                            .Buffer = { .FirstElement = 7, .NumElements = 256 } };
    ID3D12Device_CreateShaderResourceView(dev, gs, &sa, cpu_at(h, 0));
    ID3D12Device_CreateShaderResourceView(dev, gs, &sb, cpu_at(h, 1));
    ID3D12Device_CreateShaderResourceView(dev, gs, &st, cpu_at(h, 2));
    ID3D12Device_CreateUnorderedAccessView(dev, go, NULL, &uo, cpu_at(h, 3));

    ID3D12GraphicsCommandList_SetDescriptorHeaps(cl, 1, &h);
    ID3D12GraphicsCommandList_SetComputeRootSignature(cl, rs);
    ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
    ID3D12GraphicsCommandList_SetComputeRootDescriptorTable(cl, 0, gpu_at(h, 0));
    ID3D12GraphicsCommandList_Dispatch(cl, 4, 1, 1);
    const uint32_t *o = read_buf(go, sizeof zero, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    unsigned wrong = 0;
    uint32_t sum = 2166136261u;
    for (uint32_t i = 0; i < NO; i++) {
        uint32_t want = i >= 7 && i < 7 + 256 ? 111 * (i - 7) + 543 : 0;
        if (o[i] != want) { if (wrong < 3) info("view 違う out[%u] = %u（%u のはず）", i, o[i], want); wrong++; }
        sum = (sum ^ o[i]) * 16777619u;
    }
    val("view %u 個・違う %u 個・out[6] %u・out[7] %u・out[262] %u・out[263] %u・検算 %08x", NO, wrong, o[6], o[7], o[262], o[263], sum);
    if (wrong) g_bad++;
    done_read();
}

static const struct { const char *name; void (*fn)(void); } SCENES[] = {
    { "tri", scene_tri }, { "add", scene_add }, { "bind", scene_bind }, { "tex", scene_tex }, { "view", scene_view },
};

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IOFBF, 1 << 16);
    const char *want = argc > 1 ? argv[1] : "all";
    g_ppm = argc > 2 ? argv[2] : NULL;
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
    const char *hm = getenv("D3D12TEST_HEAP");
    g_heap_min = hm ? (UINT) strtoul(hm, NULL, 0) : 0;
    const char *dbg = getenv("D3D12TEST_DEBUG");
    if (dbg && *dbg == '1') {
        PFN_D3D12_GET_DEBUG_INTERFACE gd = (PFN_D3D12_GET_DEBUG_INTERFACE) (void *) GetProcAddress(d12, "D3D12GetDebugInterface");
        ID3D12Debug *d = NULL;
        if (gd && SUCCEEDED(gd(&IID_ID3D12Debug, (void **) &d))) { ID3D12Debug_EnableDebugLayer(d); info("調べる層を入れた"); }
        else info("調べる層が無い");
    }
    HRESULT hr = create(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **) &dev);
    if (FAILED(hr)) { printf("失敗 D3D12CreateDevice（0x%08lx）\n結果 まちがいあり\n", (unsigned long) hr); return 1; }
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = { 0 };
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o);
    D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1 };
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { 4, lv, 0 };
    ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl);
    info("機能の段階 %x・resource binding tier %d・tiled resources tier %d", (unsigned) fl.MaxSupportedFeatureLevel, (int) o.ResourceBindingTier, (int) o.TiledResourcesTier);
    D3D12_COMMAND_QUEUE_DESC qd = { .Type = D3D12_COMMAND_LIST_TYPE_DIRECT };
    CK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **) &queue));
    CK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **) &alloc));
    CK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **) &cl));
    CK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **) &fence));
    fence_ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    int ran = 0, bad_scenes = 0;
    for (size_t i = 0; i < sizeof SCENES / sizeof *SCENES; i++) {
        if (strcmp(want, "all") && strcmp(want, SCENES[i].name)) continue;
        g_bad = 0;
        LARGE_INTEGER f, t0, t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        SCENES[i].fn();
        QueryPerformanceCounter(&t1);
        info("%s は %.1f ms", SCENES[i].name, (double) (t1.QuadPart - t0.QuadPart) * 1000.0 / (double) f.QuadPart);
        printf("場面 %s：%s\n", SCENES[i].name, g_bad ? "違う" : "正しい");
        bad_scenes += g_bad != 0;
        ran++;
    }
    if (!ran) { printf("知らない場面 %s（tri・add・bind・tex・view・all）\n", want); return 2; }
    PROCESS_MEMORY_COUNTERS pmc = { .cb = sizeof pmc };
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) info("いちばん多く使ったメモリ %.0f MB", (double) pmc.PeakWorkingSetSize / 1048576.0);
    HRESULT rr = ID3D12Device_GetDeviceRemovedReason(dev);
    if (rr != S_OK) { printf("失敗 装置が外れた 0x%08lx\n", (unsigned long) rr); bad_scenes++; }
    printf("結果 %s\n", bad_scenes ? "まちがいあり" : "全部正しい");
    fflush(stdout);
    return bad_scenes ? 4 : 0;
}
