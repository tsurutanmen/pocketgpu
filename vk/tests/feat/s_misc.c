// 残りの場面：定数バッファの更新・計算シェーダー・間接描画・クエリ・頂点の形式・遅延コンテキスト・stream output・tessellation

static void cs_uavs(UINT n, ID3D11UnorderedAccessView *const *u, const UINT *init) { ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, n, u, init); }
static void cs_clear(void) {
    ID3D11UnorderedAccessView *nu[8] = { 0 };
    ID3D11ShaderResourceView *ns[8] = { 0 };
    ID3D11DeviceContext_CSSetUnorderedAccessViews(ctx, 0, 8, nu, NULL);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 8, ns);
}
static ID3D11UnorderedAccessView *uav_raw(ID3D11Buffer *b, UINT words) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d = { .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D11_UAV_DIMENSION_BUFFER, .Buffer = { 0, words, D3D11_BUFFER_UAV_FLAG_RAW } };
    return mk_uav(b, &d);
}
static ID3D11UnorderedAccessView *uav_struct(ID3D11Buffer *b, UINT first, UINT n, UINT flags) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC d = { .Format = DXGI_FORMAT_UNKNOWN, .ViewDimension = D3D11_UAV_DIMENSION_BUFFER, .Buffer = { first, n, flags } };
    return mk_uav(b, &d);
}
#define RAWBUF(bytes, extra, data) mk_buf((bytes), D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | (extra), 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, (data))

// ---- 6. 定数バッファの部分更新：Map DISCARD・NO_OVERWRITE を毎コマ・UpdateSubresource（全部と箱）・D3D11.1 の offset つきの束ね ----
static const char *SRC_CB =
    "cbuffer A : register(b0) { float4 arect; float4 acol; };\n"
    "cbuffer B : register(b1) { float4 frame; };\n"
    "struct VI { float2 corner : CORNER; float2 tint : TINT; };\n"
    "struct VO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "VO vsc(VI i) { VO o; o.pos = float4(lerp(arect.xy, arect.zw, i.corner), 0.5, 1); o.col = float4(acol.rgb * float3(i.tint, 1) + frame.rgb, 1); return o; }\n"
    "float4 psc(VO i) : SV_Target { return i.col; }\n";
static void scene_cbuf(void) {
    D3D11_FEATURE_DATA_D3D11_OPTIONS op = { 0 };
    ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS, &op, sizeof op);
    int d11_1 = ctx1 && op.ConstantBufferOffsetting && op.MapNoOverwriteOnDynamicConstantBuffer;
    info("D3D11.1 の offset つきの定数バッファ %d・定数バッファの NO_OVERWRITE %d", op.ConstantBufferOffsetting, op.MapNoOverwriteOnDynamicConstantBuffer);
    ID3DBlob *vb = NULL;
    ID3D11VertexShader *vs = mk_vs(SRC_CB, "vsc", &vb);
    ID3D11PixelShader *ps = mk_ps(SRC_CB, "psc");
    D3D11_INPUT_ELEMENT_DESC il[] = { { "CORNER", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
                                      { "TINT", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    ID3D11InputLayout *lay = NULL;
    CK(ID3D11Device_CreateInputLayout(dev, il, 2, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), &lay));
    ID3D11Buffer *cba = mk_buf(32, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0, NULL);
    ID3D11Buffer *cbb = mk_cb(16);
    ID3D11Buffer *big = mk_buf(4096, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0, NULL);
    enum { RING = 20 };
    ID3D11Buffer *ring = mk_buf(RING * 64, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0, NULL);
    ID3D11Buffer *dvb = mk_buf(8 * 64, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0, NULL);
    fb f = mk_fb(256, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&f, 0, 0, 0, 1);
    ID3D11DeviceContext_IASetInputLayout(ctx, lay);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
    int slot = 0;
    static const float cor[4][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
    for (int fr = 0; fr < 8; fr++) {
        float ft[4] = { fr * 0.03f, 0, fr * 0.02f, 0 };
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) cbb, 0, NULL, ft, 0, 0);
        // 左：A は Map DISCARD・頂点は輪の NO_OVERWRITE
        ID3D11Buffer *cbs[2] = { cba, cbb };
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 2, cbs);
        for (int c = 0; c < 8; c++) {
            D3D11_MAPPED_SUBRESOURCE m;
            CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) cba, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
            float a[8] = { nx(c * 16.0f), ny(fr * 16.0f), nx(c * 16.0f + 14), ny(fr * 16.0f + 14), 0.3f + 0.09f * c, 0.9f - 0.1f * fr, 0.2f + 0.1f * ((c + fr) % 7), 1 };
            memcpy(m.pData, a, sizeof a);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) cba, 0);
            int s = slot % RING;
            CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) ring, 0, s == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m));
            float *v = (float *) ((uint8_t *) m.pData + s * 64);
            for (int k = 0; k < 4; k++) { v[k * 4] = cor[k][0]; v[k * 4 + 1] = cor[k][1]; v[k * 4 + 2] = 1 - 0.1f * c + 0.05f * k; v[k * 4 + 3] = 0.5f + 0.06f * fr; }
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) ring, 0);
            UINT stride = 16, off = 0;
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &ring, &stride, &off);
            ID3D11DeviceContext_Draw(ctx, 4, (UINT) s * 4);
            slot++;
        }
        // 右：頂点は DEFAULT の箱つき UpdateSubresource・A は 1 つの大きな定数バッファを offset で束ねる
        for (int c = 0; c < 8; c++) {
            float v[16];
            for (int k = 0; k < 4; k++) { v[k * 4] = cor[k][0]; v[k * 4 + 1] = cor[k][1]; v[k * 4 + 2] = 0.6f + 0.05f * c; v[k * 4 + 3] = 1 - 0.08f * fr - 0.1f * k; }
            D3D11_BOX box = { (UINT) c * 64, 0, 0, (UINT) c * 64 + 64, 1, 1 };
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) dvb, 0, &box, v, 0, 0);
            UINT stride = 16, off = (UINT) c * 64;
            ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &dvb, &stride, &off);
            float a[8] = { nx(128 + c * 16.0f), ny(fr * 16.0f), nx(128 + c * 16.0f + 14), ny(fr * 16.0f + 14), 0.9f - 0.08f * c, 0.3f + 0.08f * fr, 0.5f, 1 };
            D3D11_MAPPED_SUBRESOURCE m;
            if (d11_1) {
                CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) big, 0, c == 0 ? D3D11_MAP_WRITE_DISCARD : D3D11_MAP_WRITE_NO_OVERWRITE, 0, &m));
                memcpy((uint8_t *) m.pData + c * 256, a, sizeof a);
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) big, 0);
                UINT first = (UINT) c * 16, num = 16;
                ID3D11DeviceContext1_VSSetConstantBuffers1(ctx1, 0, 1, &big, &first, &num);
            } else {
                CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) cba, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
                memcpy(m.pData, a, sizeof a);
                ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) cba, 0);
                ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cba);
            }
            ID3D11DeviceContext_Draw(ctx, 4, 0);
        }
        ID3D11DeviceContext_Flush(ctx);
    }
    img m = show(f.ct, 0, 0, 0, 0, "定数バッファの更新 8 コマ");
    for (int fr = 0; fr < 8; fr += 3)
        for (int c = 0; c < 16; c += 5) val_px(&m, c * 16 + 7, fr * 16 + 7, "コマ");
}

// ---- 7. 計算シェーダー：UAV の読み書き（テクスチャ・型つき・float）と、書いたテクスチャを SRV で読む ----
static const char *SRC_CU1 =
    "RWTexture2D<unorm float4> ot : register(u0); RWBuffer<uint> bu : register(u1); RWTexture2D<float> rf : register(u2);\n"
    "[numthreads(8, 8, 1)] void cs(uint3 id : SV_DispatchThreadID) {\n"
    "  ot[id.xy] = float4(id.x / 127.0, id.y / 127.0, ((id.x ^ id.y) & 15) / 15.0, 1);\n"
    "  uint i = id.y * 128 + id.x; bu[i] = bu[i] * 3 + i; rf[id.xy] = rf[id.xy] * 2 + id.x; }\n"
    "Texture2D<float4> it : register(t0); RWByteAddressBuffer ob : register(u0);\n"
    "[numthreads(64, 1, 1)] void cs2(uint3 id : SV_DispatchThreadID) { float4 c = it.Load(int3(id.x % 128, id.x / 128, 0));\n"
    "  ob.Store(id.x * 4, (uint) (c.r * 255 + 0.5) | ((uint) (c.b * 255 + 0.5) << 8)); }\n";
static void scene_cs_uav(void) {
    enum { S = 128 };
    ID3D11Texture2D *ot = mk_tex(S, S, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, NULL);
    uint32_t *bi = malloc(S * S * 4);
    float *fi = malloc(S * S * 4);
    for (int i = 0; i < S * S; i++) { bi[i] = (uint32_t) i; fi[i] = (float) (i % S) * 0.25f; }
    ID3D11Buffer *bu = mk_buf(S * S * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0, bi);
    D3D11_UNORDERED_ACCESS_VIEW_DESC bd = { .Format = DXGI_FORMAT_R32_UINT, .ViewDimension = D3D11_UAV_DIMENSION_BUFFER, .Buffer = { 0, S * S, 0 } };
    D3D11_SUBRESOURCE_DATA ifi = { fi, S * 4, 0 };
    ID3D11Texture2D *rf = mk_tex(S, S, 1, 1, DXGI_FORMAT_R32_FLOAT, 1, D3D11_BIND_UNORDERED_ACCESS, 0, &ifi);
    ID3D11UnorderedAccessView *u[3] = { mk_uav(ot, NULL), mk_uav(bu, &bd), mk_uav(rf, NULL) };
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_CU1, "cs"), NULL, 0);
    cs_uavs(3, u, NULL);
    ID3D11DeviceContext_Dispatch(ctx, S / 8, S / 8, 1);
    ID3D11DeviceContext_Dispatch(ctx, S / 8, S / 8, 1);
    cs_clear();
    // 書いたテクスチャを SRV で読んでバイトのバッファへ
    ID3D11Buffer *ob = RAWBUF(S * S * 4, 0, NULL);
    ID3D11UnorderedAccessView *ou = uav_raw(ob, S * S);
    ID3D11ShaderResourceView *is = mk_srv(ot, NULL);
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_CU1, "cs2"), NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &is);
    cs_uavs(1, &ou, NULL);
    ID3D11DeviceContext_Dispatch(ctx, S * S / 64, 1, 1);
    cs_clear();
    show(ot, 0, 0, 0, 0, "UAV に書いたテクスチャ");
    uint32_t *rb = read_buf(bu, S * S * 4);
    int bad = 0;
    for (int i = 0; i < S * S; i++) if (rb[i] != 13u * (uint32_t) i) bad++;
    val("RWBuffer<uint> を 2 回読み書き：違う %d / %d", bad, S * S);
    img mf = read_tex(rf, 0, 0, 0);
    bad = 0;
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) {
            float w = ((x * 0.25f) * 2 + x) * 2 + x, g;
            memcpy(&g, mf.px + (y * S + x) * 4, 4);
            if (fabsf(g - w) > 1e-4f) bad++;
        }
    val("RWTexture2D<float> を 2 回読み書き：違う %d", bad);
    uint32_t *o2 = read_buf(ob, S * S * 4);
    bad = 0;
    for (int i = 0; i < S * S; i++) {
        int x = i % S, y = i / S;
        uint32_t r = (uint32_t) to8(x / 127.0f), b = (uint32_t) to8(((x ^ y) & 15) / 15.0f);
        int gr = (int) (o2[i] & 255), gb = (int) ((o2[i] >> 8) & 255);
        if (abs(gr - (int) r) > 1 || abs(gb - (int) b) > 1 || (o2[i] >> 16)) bad++;
    }
    val("UAV で書いたテクスチャを SRV で読む：違う %d", bad);
}

// ---- 7. 原子演算 ----
static const char *SRC_AT =
    "RWByteAddressBuffer cnt : register(u0); RWStructuredBuffer<uint> slots : register(u1); RWTexture2D<uint> im : register(u2);\n"
    "[numthreads(64, 1, 1)] void cs(uint3 id : SV_DispatchThreadID) { uint i = id.x, old;\n"
    "  cnt.InterlockedAdd(((i * 7) % 16) * 4, 1);\n"
    "  cnt.InterlockedAdd(16 * 4, 1, old); slots[old] = i + 1;\n"
    "  cnt.InterlockedMax(17 * 4, (i * 2654435761u) >> 8);\n"
    "  cnt.InterlockedMin(18 * 4, (i * 40503u) ^ 0x5555u);\n"
    "  cnt.InterlockedOr(19 * 4, 1u << (i % 32));\n"
    "  cnt.InterlockedXor(20 * 4, i * 2246822519u);\n"
    "  uint a = (32 + i % 32) * 4, v = cnt.Load(a);\n"
    "  [allow_uav_condition] for (int k = 0; k < 100000; k++) { uint prev; cnt.InterlockedCompareExchange(a, v, v + i, prev); if (prev == v) break; v = prev; }\n"
    "  InterlockedAdd(im[uint2(i % 16, (i / 16) % 16)], i); }\n";
static void scene_cs_atomic(void) {
    enum { N = 65536 };
    uint32_t init[64] = { 0 };
    init[18] = 0xffffffffu;
    ID3D11Buffer *cnt = RAWBUF(256, 0, init);
    ID3D11Buffer *slots = mk_buf(N * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, NULL);
    ID3D11Texture2D *im = mk_tex(16, 16, 1, 1, DXGI_FORMAT_R32_UINT, 1, D3D11_BIND_UNORDERED_ACCESS, 0, NULL);
    ID3D11UnorderedAccessView *u[3] = { uav_raw(cnt, 64), uav_struct(slots, 0, N, 0), mk_uav(im, NULL) };
    UINT zero4[4] = { 0 };
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(ctx, u[1], zero4);
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(ctx, u[2], zero4);
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_AT, "cs"), NULL, 0);
    cs_uavs(3, u, NULL);
    ID3D11DeviceContext_Dispatch(ctx, N / 64, 1, 1);
    cs_clear();
    uint32_t *c = read_buf(cnt, 256), *s = read_buf(slots, N * 4);
    img mi = read_tex(im, 0, 0, 0);
    uint32_t mx = 0, mn = 0xffffffffu, orv = 0, xr = 0, cas[32] = { 0 }, ia[256] = { 0 };
    for (uint32_t i = 0; i < N; i++) {
        uint32_t a = (i * 2654435761u) >> 8, b = (i * 40503u) ^ 0x5555u;
        if (a > mx) mx = a;
        if (b < mn) mn = b;
        orv |= 1u << (i % 32);
        xr ^= i * 2246822519u;
        cas[i % 32] += i;
        ia[(i % 16) + 16 * ((i / 16) % 16)] += i;
    }
    int bad = 0;
    for (int b = 0; b < 16; b++) if (c[b] != N / 16) bad++;
    val("InterlockedAdd の 16 の箱：違う箱 %d（[0] = %u）", bad, c[0]);
    val("InterlockedAdd の戻り値の数 %u", c[16]);
    uint8_t *seen = calloc(N + 1, 1);
    int dup = 0, miss = 0;
    for (int i = 0; i < N; i++) { if (s[i] == 0 || s[i] > N) miss++; else if (seen[s[i]]++) dup++; }
    val("戻り値で取った席：重なり %d・空き %d", dup, miss);
    val("InterlockedMax %s（%08x）・Min %s・Or %s・Xor %s", c[17] == mx ? "合う" : "違う", c[17], c[18] == mn ? "合う" : "違う", c[19] == orv ? "合う" : "違う", c[20] == xr ? "合う" : "違う");
    bad = 0;
    for (int k = 0; k < 32; k++) if (c[32 + k] != cas[k]) bad++;
    val("InterlockedCompareExchange の足し算：違う %d / 32", bad);
    bad = 0;
    for (int k = 0; k < 256; k++) if (img_u32(&mi, k % 16, k / 16) != ia[k]) bad++;
    val("テクスチャの InterlockedAdd：違う %d / 256", bad);
}

// ---- 7. groupshared：グループの中の累積和・SV_GroupIndex ----
static const char *SRC_GS =
    "StructuredBuffer<uint> inp : register(t0); RWStructuredBuffer<uint> outp : register(u0); RWStructuredBuffer<uint> scan : register(u1);\n"
    "groupshared uint sh[256];\n"
    "[numthreads(256, 1, 1)] void cs(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint3 d : SV_DispatchThreadID) {\n"
    "  sh[t.x] = inp[d.x]; GroupMemoryBarrierWithGroupSync();\n"
    "  for (uint off = 1; off < 256; off <<= 1) { uint v = t.x >= off ? sh[t.x - off] : 0; GroupMemoryBarrierWithGroupSync(); sh[t.x] += v; GroupMemoryBarrierWithGroupSync(); }\n"
    "  scan[d.x] = sh[t.x]; if (t.x == 255) outp[g.x] = sh[255]; }\n"
    "RWStructuredBuffer<uint4> gi : register(u0);\n"
    "groupshared float4 m4[64];\n"
    "[numthreads(4, 4, 4)] void cs3(uint3 g : SV_GroupID, uint3 t : SV_GroupThreadID, uint gidx : SV_GroupIndex) {\n"
    "  uint gf = g.x + g.y * 3 + g.z * 6; m4[gidx] = float4(t, gf); GroupMemoryBarrierWithGroupSync();\n"
    "  float4 o = m4[63 - gidx]; gi[gf * 64 + gidx] = uint4(o); }\n";
static void scene_cs_shared(void) {
    enum { G = 256, N = G * 256 };
    uint32_t *in = malloc(N * 4);
    for (int i = 0; i < N; i++) in[i] = (uint32_t) ((i * 2654435761u) >> 20);
    ID3D11Buffer *ib = mk_buf(N * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, in);
    ID3D11Buffer *ob = mk_buf(G * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, NULL);
    ID3D11Buffer *sb = mk_buf(N * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, NULL);
    ID3D11ShaderResourceView *sv = mk_srv(ib, NULL);
    ID3D11UnorderedAccessView *u[2] = { uav_struct(ob, 0, G, 0), uav_struct(sb, 0, N, 0) };
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_GS, "cs"), NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &sv);
    cs_uavs(2, u, NULL);
    ID3D11DeviceContext_Dispatch(ctx, G, 1, 1);
    cs_clear();
    uint32_t *o = read_buf(ob, G * 4), *s = read_buf(sb, N * 4);
    int bg = 0, bs = 0;
    for (int g = 0; g < G; g++) {
        uint32_t acc = 0;
        for (int t = 0; t < 256; t++) { acc += in[g * 256 + t]; if (s[g * 256 + t] != acc) bs++; }
        if (o[g] != acc) bg++;
    }
    val("groupshared の和：違うグループ %d / %d・累積和の違い %d / %d", bg, G, bs, N);
    ID3D11Buffer *gb = mk_buf(12 * 64 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 16, NULL);
    ID3D11UnorderedAccessView *gu = uav_struct(gb, 0, 12 * 64, 0);
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_GS, "cs3"), NULL, 0);
    cs_uavs(1, &gu, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 3, 2, 2);
    cs_clear();
    uint32_t *r = read_buf(gb, 12 * 64 * 16);
    int bad = 0;
    for (int gf = 0; gf < 12; gf++)
        for (int gi = 0; gi < 64; gi++) {
            int src = 63 - gi, tx = src % 4, ty = (src / 4) % 4, tz = src / 16;
            const uint32_t *q = r + (gf * 64 + gi) * 4;
            if (q[0] != (uint32_t) tx || q[1] != (uint32_t) ty || q[2] != (uint32_t) tz || q[3] != (uint32_t) gf) bad++;
        }
    val("SV_GroupIndex と groupshared の float4：違う %d / 768", bad);
}

// ---- 7. 構造化バッファ（半端な stride）と、範囲の外へ書かないこと ----
static const char *SRC_ST =
    "struct In { float3 p; uint id; float2 uv; };\n"
    "struct Out { float a; uint3 b; float2 c; float d; uint e; float f; };\n"
    "StructuredBuffer<In> si : register(t0); RWStructuredBuffer<Out> so : register(u0);\n"
    "[numthreads(64, 1, 1)] void cs(uint3 d : SV_DispatchThreadID) { In x = si[d.x]; Out o;\n"
    "  o.a = x.p.x + x.p.y * 2 + x.p.z * 3; o.b = uint3(x.id, x.id * 2, d.x); o.c = x.uv * 2; o.d = x.p.x * x.p.y; o.e = x.id ^ 0xabcd; o.f = x.uv.x - x.uv.y;\n"
    "  so[d.x] = o; }\n";
static void scene_cs_struct(void) {
    enum { N = 1000, NB = 1024 };
    struct in { float p[3]; uint32_t id; float uv[2]; } *in = calloc(NB, sizeof *in);
    struct out { float a; uint32_t b[3]; float c[2]; float d; uint32_t e; float f; } *ini = calloc(NB, sizeof *ini);
    for (int i = 0; i < NB; i++) {
        in[i].p[0] = i * 0.5f; in[i].p[1] = 3 - i * 0.25f; in[i].p[2] = (i % 7) * 1.5f; in[i].id = (uint32_t) (i * 31 + 7); in[i].uv[0] = i / 1000.0f; in[i].uv[1] = 1 - i / 500.0f;
        memset(&ini[i], 0xcd, sizeof ini[i]);
    }
    info("stride：入り %zu・出し %zu", sizeof *in, sizeof *ini);
    ID3D11Buffer *ib = mk_buf(N * sizeof *in, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof *in, in);
    ID3D11Buffer *ob = mk_buf(NB * sizeof *ini, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, sizeof *ini, ini);
    ID3D11ShaderResourceView *sv = mk_srv(ib, NULL);
    ID3D11UnorderedAccessView *u = uav_struct(ob, 0, N, 0); // 1000 個の view・後ろの 24 個は印のまま残るはず
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_ST, "cs"), NULL, 0);
    ID3D11DeviceContext_CSSetShaderResources(ctx, 0, 1, &sv);
    cs_uavs(1, &u, NULL);
    ID3D11DeviceContext_Dispatch(ctx, NB / 64, 1, 1);
    cs_clear();
    struct out *o = read_buf(ob, NB * sizeof *ini);
    int bad = 0;
    for (int i = 0; i < N; i++) {
        struct in *x = &in[i];
        struct out *q = &o[i];
        float a = x->p[0] + x->p[1] * 2 + x->p[2] * 3;
        if (fabsf(q->a - a) > 1e-3f * (1 + fabsf(a)) || q->b[0] != x->id || q->b[1] != x->id * 2 || q->b[2] != (uint32_t) i || fabsf(q->c[0] - x->uv[0] * 2) > 1e-5f ||
            fabsf(q->c[1] - x->uv[1] * 2) > 1e-5f || fabsf(q->d - x->p[0] * x->p[1]) > 1e-3f * (1 + fabsf(q->d)) || q->e != (x->id ^ 0xabcd) || fabsf(q->f - (x->uv[0] - x->uv[1])) > 1e-5f)
            bad++;
    }
    val("構造化バッファ（stride 24 → 36）：違う %d / %d", bad, N);
    int outside = 0;
    for (int i = N; i < NB; i++) if (memcmp(&o[i], &ini[i], sizeof ini[i])) outside++;
    val("view の外（後ろの 24 個）を書き換えた数 %d", outside);
}

// ---- 7. 追記バッファ（Append/Consume）・カウンタつき・CopyStructureCount ----
static const char *SRC_AP =
    "AppendStructuredBuffer<uint2> ap : register(u0);\n"
    "[numthreads(64, 1, 1)] void csa(uint3 d : SV_DispatchThreadID) { if (d.x % 3 == 0) ap.Append(uint2(d.x, d.x * d.x)); }\n"
    "ConsumeStructuredBuffer<uint2> co : register(u0); RWByteAddressBuffer sum : register(u1);\n"
    "[numthreads(64, 1, 1)] void csc(uint3 d : SV_DispatchThreadID) { if (d.x < 500) { uint2 v = co.Consume(); sum.InterlockedAdd(0, v.x % 3 == 0 && v.y == v.x * v.x ? 1 : 0); sum.InterlockedAdd(4, 1); } }\n"
    "RWStructuredBuffer<uint> cn : register(u0);\n"
    "[numthreads(64, 1, 1)] void csn(uint3 d : SV_DispatchThreadID) { if (d.x & 1) { uint k = cn.IncrementCounter(); cn[k] = d.x; } }\n";
static uint32_t struct_count(ID3D11UnorderedAccessView *u) {
    ID3D11Buffer *c = RAWBUF(16, 0, NULL);
    ID3D11DeviceContext_CopyStructureCount(ctx, c, 4, u);
    uint32_t *r = read_buf(c, 16);
    return r[1];
}
static void scene_cs_append(void) {
    enum { N = 4096 };
    ID3D11Buffer *ab = mk_buf(N * 8, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 8, NULL);
    ID3D11UnorderedAccessView *au = uav_struct(ab, 0, N, D3D11_BUFFER_UAV_FLAG_APPEND);
    UINT zero = 0, keep = (UINT) -1, ten = 10;
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_AP, "csa"), NULL, 0);
    cs_uavs(1, &au, &zero);
    ID3D11DeviceContext_Dispatch(ctx, N / 64, 1, 1);
    cs_clear();
    uint32_t n1 = struct_count(au);
    val("Append の数 %u（1366 のはず）", n1);
    uint32_t *a = read_buf(ab, N * 8);
    uint8_t *seen = calloc(N, 1);
    int bad = 0;
    for (uint32_t i = 0; i < n1 && i < N; i++) {
        uint32_t x = a[i * 2], y = a[i * 2 + 1];
        if (x >= N || x % 3 || y != x * x || seen[x]++) bad++;
    }
    val("Append で入った物：おかしい %d", bad);
    // Consume：今の数のまま 500 個取る
    ID3D11Buffer *sum = RAWBUF(16, 0, NULL);
    ID3D11UnorderedAccessView *su = uav_raw(sum, 4);
    UINT z4[4] = { 0 };
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(ctx, su, z4);
    ID3D11UnorderedAccessView *cu[2] = { au, su };
    UINT ic[2] = { keep, keep };
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_AP, "csc"), NULL, 0);
    cs_uavs(2, cu, ic);
    ID3D11DeviceContext_Dispatch(ctx, 8, 1, 1);
    cs_clear();
    uint32_t *sr = read_buf(sum, 16);
    val("Consume：取った %u・正しい物 %u・残り %u（866 のはず）", sr[1], sr[0], struct_count(au));
    // 始めの数を 10 にして 64 個のうち 22 個を Append → 32
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_AP, "csa"), NULL, 0);
    cs_uavs(1, &au, &ten);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    cs_clear();
    val("始めの数 10 から Append 22 個：%u", struct_count(au));
    // カウンタつきの構造化バッファ
    ID3D11Buffer *nb = mk_buf(N * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, NULL);
    ID3D11UnorderedAccessView *nu = uav_struct(nb, 0, N, D3D11_BUFFER_UAV_FLAG_COUNTER);
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_AP, "csn"), NULL, 0);
    cs_uavs(1, &nu, &zero);
    ID3D11DeviceContext_Dispatch(ctx, N / 64, 1, 1);
    cs_clear();
    uint32_t n2 = struct_count(nu);
    uint32_t *nr = read_buf(nb, N * 4);
    memset(seen, 0, N);
    bad = 0;
    for (uint32_t i = 0; i < n2 && i < N; i++) if (nr[i] >= N || !(nr[i] & 1) || seen[nr[i]]++) bad++;
    val("IncrementCounter：数 %u（2048 のはず）・おかしい %d", n2, bad);
}

// ---- 8. 間接描画と間接の計算 ----
static const char *SRC_IN = Q_HEAD
    "VO vsi(uint id : SV_VertexID, uint inst : SV_InstanceID) { float2 t = float2(id & 1, id >> 1); float2 p = lerp(rect.xy, rect.zw, t);\n"
    "  p.x += inst * ex.y; VO o; o.pos = float4(p, 0.5, 1); o.col = col * (1 - 0.1 * inst); o.uv = t; return o; }\n"
    "RWByteAddressBuffer args : register(u0);\n"
    "[numthreads(1, 1, 1)] void csw(uint3 d : SV_DispatchThreadID) { args.Store4(0, uint4(6, 5, 0, 0)); args.Store(16, 0); args.Store3(32, uint3(4, 2, 1)); }\n"
    "RWByteAddressBuffer cnt : register(u0);\n"
    "[numthreads(8, 8, 1)] void csd(uint3 g : SV_GroupID) { cnt.InterlockedAdd(0, 1); cnt.InterlockedAdd(4 + 4 * (g.x + 4 * g.y), 1); }\n"
    "AppendStructuredBuffer<uint> ap : register(u0);\n"
    "[numthreads(64, 1, 1)] void csa(uint3 d : SV_DispatchThreadID) { if (d.x < 7) ap.Append(d.x); }\n";
static void scene_indirect(void) {
    ID3D11VertexShader *vs = mk_vs(SRC_IN, "vsi", NULL);
    uint32_t a0[16] = { 4, 3, 0, 0, 0, 6, 2, 0, 0, 0 }; // 0：DrawInstanced の 4 つ・20：DrawIndexedInstanced の 5 つ
    ID3D11Buffer *args = mk_buf(64, D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, a0);
    ID3D11Buffer *gargs = mk_buf(64, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, NULL);
    uint16_t ix[6] = { 0, 1, 2, 2, 1, 3 };
    ID3D11Buffer *ib = mk_buf(12, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0, ix);
    // GPU が引数を書く
    ID3D11UnorderedAccessView *gu = uav_raw(gargs, 16);
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_IN, "csw"), NULL, 0);
    cs_uavs(1, &gu, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    cs_clear();
    // Append の数を instance の数へ写す
    ID3D11Buffer *apb = mk_buf(64 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4, NULL);
    ID3D11UnorderedAccessView *apu = uav_struct(apb, 0, 64, D3D11_BUFFER_UAV_FLAG_APPEND);
    UINT zero = 0;
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_IN, "csa"), NULL, 0);
    cs_uavs(1, &apu, &zero);
    ID3D11DeviceContext_Dispatch(ctx, 1, 1, 1);
    cs_clear();
    uint32_t a2[4] = { 4, 0, 0, 0 };
    ID3D11Buffer *cargs = mk_buf(16, D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0, a2);
    ID3D11DeviceContext_CopyStructureCount(ctx, cargs, 4, apu);
    fb f = mk_fb(256, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&f, 0, 0, 0, 1);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    float step = 32.0f / 128.0f;
    float cb[16] = { nx(4), ny(4), nx(28), ny(28), 1, 0.3f, 0.3f, 1, 0.5f, 0.5f, 0, 0, 0, step, 0, 0 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_DrawInstancedIndirect(ctx, args, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
    cb[1] = ny(36); cb[3] = ny(60); cb[4] = 0.3f; cb[5] = 1; cb[6] = 0.3f;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_DrawIndexedInstancedIndirect(ctx, args, 20);
    cb[1] = ny(68); cb[3] = ny(92); cb[4] = 0.3f; cb[5] = 0.3f; cb[6] = 1;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_DrawIndexedInstancedIndirect(ctx, gargs, 0);
    cb[1] = ny(100); cb[3] = ny(124); cb[4] = 1; cb[5] = 1; cb[6] = 0.3f;
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_DrawInstancedIndirect(ctx, cargs, 0);
    img m = show(f.ct, 0, 0, 0, 0, "間接描画 4 段");
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) {
            float o[4];
            img_get(&m, 16 + k * 32, 16 + r * 32, o);
            if (k == 7 || o[0] + o[1] + o[2] < 0.1f) { val("間接描画 段 %d：%d 個", r, k); break; }
        }
    // DispatchIndirect（引数は GPU が書いた 4x2x1）
    ID3D11Buffer *cnt = RAWBUF(64, 0, NULL);
    ID3D11UnorderedAccessView *cu = uav_raw(cnt, 16);
    UINT z4[4] = { 0 };
    ID3D11DeviceContext_ClearUnorderedAccessViewUint(ctx, cu, z4);
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_IN, "csd"), NULL, 0);
    cs_uavs(1, &cu, NULL);
    ID3D11DeviceContext_DispatchIndirect(ctx, gargs, 32);
    cs_clear();
    uint32_t *c = read_buf(cnt, 64);
    val("DispatchIndirect：糸の数 %u（512 のはず）・グループごと %u %u %u %u %u %u %u %u", c[0], c[1], c[2], c[3], c[4], c[5], c[6], c[7], c[8]);
}

// ---- 9. クエリ：occlusion（数と predicate）・EVENT・timestamp と disjoint・pipeline statistics ----
static const char *SRC_PS =
    "struct VO { float4 pos : SV_Position; };\n"
    "VO vsm(uint id : SV_VertexID) { uint t = id / 3, k = id % 3; float2 b = float2(t % 10, t / 10) * 0.2 - 0.95;\n"
    "  VO o; o.pos = float4(b + float2(k == 1, k == 2) * 0.12, 0.5, 1); return o; }\n"
    "float4 psm(VO i) : SV_Target { return float4(1, 1, 0, 1); }\n"
    "RWByteAddressBuffer cnt : register(u0);\n"
    "[numthreads(64, 1, 1)] void csm(uint3 d : SV_DispatchThreadID) { cnt.InterlockedAdd(0, 1); }\n";
static void scene_query(void) {
    fb f = mk_fb(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT, 1);
    fb_begin(&f, 0, 0, 0, 1);
    ID3D11DepthStencilState *less = mk_dss(TRUE, TRUE, D3D11_COMPARISON_LESS, 0, 0, 0xff, 0xff);
    set_ds(less, 0);
    quad(0, 0, 64, 128, 0.3f, 0.3f, 0.3f, 1, 0.3f, 0.3f, 0);
    D3D11_QUERY_DESC qo = { D3D11_QUERY_OCCLUSION, 0 }, qp = { D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
    ID3D11Query *q[3];
    ID3D11Predicate *p[2];
    for (int i = 0; i < 3; i++) CK(ID3D11Device_CreateQuery(dev, &qo, &q[i]));
    for (int i = 0; i < 2; i++) CK(ID3D11Device_CreatePredicate(dev, &qp, &p[i]));
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) q[0]);
    quad(32, 32, 96, 96, 1, 0.2f, 0.2f, 1, 0.5f, 0.5f, 0); // 右半分の 32x64 = 2048 だけ見える
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) q[0]);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) q[1]);
    quad(8, 8, 56, 56, 1, 0.2f, 0.2f, 1, 0.5f, 0.5f, 0); // 全部隠れる
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) q[1]);
    set_ds(mk_dss(FALSE, FALSE, 0, 0, 0, 0xff, 0xff), 0);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) q[2]);
    quad(10, 100, 110, 110, 0.2f, 0.2f, 1, 1, 0.5f, 0.5f, 0); // 100x10 = 1000
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) q[2]);
    set_ds(less, 0);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) p[0]);
    quad(8, 8, 56, 56, 1, 1, 1, 1, 0.5f, 0.5f, 0); // 隠れる
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) p[0]);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) p[1]);
    quad(70, 8, 120, 30, 0.6f, 0.6f, 0.6f, 1, 0.5f, 0.5f, 0); // 見える
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) p[1]);
    set_ds(mk_dss(FALSE, FALSE, 0, 0, 0, 0xff, 0xff), 0);
    ID3D11DeviceContext_SetPredication(ctx, p[0], FALSE);
    quad(100, 40, 124, 60, 1, 0, 1, 1, 0.5f, 0.5f, 0); // 飛ばされるはず
    ID3D11DeviceContext_SetPredication(ctx, p[1], FALSE);
    quad(100, 64, 124, 84, 0, 1, 0, 1, 0.5f, 0.5f, 0); // 描かれるはず
    ID3D11DeviceContext_SetPredication(ctx, NULL, FALSE);
    for (int i = 0; i < 3; i++) {
        UINT64 n = 0;
        int r = get_data(q[i], &n, sizeof n);
        val("occlusion %d：%s %llu", i, r == 1 ? "返った" : r == 0 ? "時間切れ" : "失敗", (unsigned long long) n);
    }
    for (int i = 0; i < 2; i++) {
        BOOL b = 2;
        int r = get_data(p[i], &b, sizeof b);
        val("predicate %d：%s %d", i, r == 1 ? "返った" : r == 0 ? "時間切れ" : "失敗", (int) b);
    }
    img m = show(f.ct, 0, 0, 0, 0, "occlusion と predicate");
    val_px(&m, 112, 50, "predicate で飛ばすはずの所");
    val_px(&m, 112, 74, "predicate で描くはずの所");
    // EVENT
    D3D11_QUERY_DESC qe = { D3D11_QUERY_EVENT, 0 };
    ID3D11Query *ev = NULL;
    CK(ID3D11Device_CreateQuery(dev, &qe, &ev));
    int ok = 0;
    for (int i = 0; i < 5; i++) {
        clear_rtv(f.rtv, i * 0.2f, 0, 0, 1);
        ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) ev);
        BOOL done = FALSE;
        if (get_data(ev, &done, sizeof done) == 1 && done) ok++;
    }
    val("EVENT：5 回中 %d 回返った", ok);
    // timestamp と disjoint
    D3D11_QUERY_DESC qd = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0 }, qt = { D3D11_QUERY_TIMESTAMP, 0 };
    ID3D11Query *dj = NULL, *t0 = NULL, *t1 = NULL;
    CK(ID3D11Device_CreateQuery(dev, &qd, &dj));
    CK(ID3D11Device_CreateQuery(dev, &qt, &t0));
    CK(ID3D11Device_CreateQuery(dev, &qt, &t1));
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) dj);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) t0);
    fb_begin(&f, 0, 0, 0, 1);
    for (int i = 0; i < 200; i++) quad(0, 0, 128, 128, i / 200.0f, 0.5f, 0.2f, 1, 0.5f, 0.5f, 0);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) t1);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) dj);
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dd = { 0 };
    UINT64 v0 = 0, v1 = 0;
    int r0 = get_data(t0, &v0, sizeof v0), r1 = get_data(t1, &v1, sizeof v1), rd = get_data(dj, &dd, sizeof dd);
    val("timestamp：返った %d %d %d・disjoint %d・周波数が 0 より大きい %s・後が前より大きい %s", r0, r1, rd, (int) dd.Disjoint, dd.Frequency > 0 ? "はい" : "いいえ", v1 > v0 ? "はい" : "いいえ");
    double ms = dd.Frequency ? (double) (v1 - v0) * 1000.0 / (double) dd.Frequency : -1;
    info("timestamp：周波数 %llu・間 %.3f ms（%llu → %llu）", (unsigned long long) dd.Frequency, ms, (unsigned long long) v0, (unsigned long long) v1);
    val("timestamp の間が 0〜1000 ms の中 %s", ms >= 0 && ms < 1000 ? "はい" : "いいえ");
    // pipeline statistics
    D3D11_QUERY_DESC qs = { D3D11_QUERY_PIPELINE_STATISTICS, 0 };
    ID3D11Query *ps = NULL, *ps2 = NULL;
    CK(ID3D11Device_CreateQuery(dev, &qs, &ps));
    CK(ID3D11Device_CreateQuery(dev, &qs, &ps2));
    fb_begin(&f, 0, 0, 0, 1);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) ps);
    ID3D11DeviceContext_VSSetShader(ctx, mk_vs(SRC_PS, "vsm", NULL), NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, mk_ps(SRC_PS, "psm"), NULL, 0);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_Draw(ctx, 300, 0);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) ps);
    ID3D11Buffer *cnt = RAWBUF(16, 0, NULL);
    ID3D11UnorderedAccessView *cu = uav_raw(cnt, 4);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) ps2);
    ID3D11DeviceContext_CSSetShader(ctx, mk_cs(SRC_PS, "csm"), NULL, 0);
    cs_uavs(1, &cu, NULL);
    ID3D11DeviceContext_Dispatch(ctx, 4, 1, 1);
    cs_clear();
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) ps2);
    D3D11_QUERY_DATA_PIPELINE_STATISTICS s = { 0 }, s2 = { 0 };
    int rs = get_data(ps, &s, sizeof s), rs2 = get_data(ps2, &s2, sizeof s2);
    val("pipeline statistics（描画）：返った %d・IAVertices %llu・IAPrimitives %llu・VSInvocations %llu・CInvocations %llu・CPrimitives %llu・PSInvocations %llu・CSInvocations %llu",
        rs, (unsigned long long) s.IAVertices, (unsigned long long) s.IAPrimitives, (unsigned long long) s.VSInvocations, (unsigned long long) s.CInvocations,
        (unsigned long long) s.CPrimitives, (unsigned long long) s.PSInvocations, (unsigned long long) s.CSInvocations);
    val("pipeline statistics（計算）：返った %d・CSInvocations %llu・IAVertices %llu", rs2, (unsigned long long) s2.CSInvocations, (unsigned long long) s2.IAVertices);
    show(f.ct, 0, 0, 0, 0, "statistics で描いた 100 の三角");
}

// ---- 14. 頂点の形式いろいろ・インスタンスごとの頂点（step rate 1・2・3・0）----
#pragma pack(push, 1)
struct vf { int16_t pos[2]; uint8_t c0[4]; uint32_t c1; uint16_t c2[4]; uint8_t c3[4]; int8_t c4[4]; uint16_t c5[2]; uint8_t c6[4]; uint32_t c7; float c8[3]; int16_t c9[2]; uint8_t c10[2], pad[2]; uint32_t c11; };
#pragma pack(pop)
static const char *SRC_VF = Q_HEAD
    "struct VI { float2 pos : POSITION; float4 c0 : C0; float4 c1 : C1; float4 c2 : C2; float4 c3 : C3; float4 c4 : C4; float2 c5 : C5; uint4 c6 : C6;\n"
    "            uint4 c7 : C7; float3 c8 : C8; int2 c9 : C9; float2 c10 : C10; uint c11 : C11; };\n"
    "VO vsv(VI i) { VO o; uint k = (uint) ex.x; float2 cell = float2(k % 4, k / 4);\n"
    "  float2 p = float2(cell.x * 64 + (i.pos.x * 0.5 + 0.5) * 64, cell.y * 64 + (0.5 - i.pos.y * 0.5) * 64);\n"
    "  o.pos = float4(p.x / 128 - 1, 1 - p.y / 128, 0.5, 1); float3 c = 0;\n"
    "  if (k == 0) c = i.c0.rgb * 0.8 + i.c0.a * 0.2; else if (k == 1) c = float3(i.c1.r, i.c1.g, i.c1.b * 0.7 + i.c1.a * 0.3);\n"
    "  else if (k == 2) c = i.c2.rgb * 0.5 + i.c2.a * 0.1; else if (k == 3) c = float3(i.c3.r, i.c3.g, i.c3.b * 0.7 + i.c3.a * 0.3);\n"
    "  else if (k == 4) c = i.c4.rgb * 0.4 + 0.5 + i.c4.a * 0.1; else if (k == 5) c = float3(i.c5, 0.5);\n"
    "  else if (k == 6) c = (i.c6.rgb + i.c6.a * 4) / 255.0; else if (k == 7) c = float3(i.c7.r, i.c7.g, i.c7.b * 0.7) / 1023.0 + i.c7.a * 0.1;\n"
    "  else if (k == 8) c = i.c8; else if (k == 9) c = float3(i.c9 / 2000.0 + 0.5, 0.3); else if (k == 10) c = float3(i.c10, 0.2);\n"
    "  else c = float3(i.c11 & 255, (i.c11 >> 8) & 255, (i.c11 >> 16) & 255) / 255.0;\n"
    "  o.col = float4(c, 1); o.uv = 0; return o; }\n"
    "struct VJ { float2 corner : CORNER; float2 off : OFF; float4 col : ICOL; float sc : ISC; };\n"
    "VO vsj(VJ i) { VO o; float2 p = float2(i.off.x, i.off.y + ex.y) + i.corner * 12 * i.sc; o.pos = float4(p.x / 128 - 1, 1 - p.y / 128, 0.5, 1); o.col = i.col; o.uv = 0; return o; }\n";
static void scene_vfmt(void) {
    enum { NV = 48 };
    struct vf v[NV];
    memset(v, 0, sizeof v);
    for (int i = 0; i < NV; i++) {
        int t = i / 3, k = i % 3, gx = t % 4, gy = t / 4;
        float x = -0.9f + gx * 0.47f + (k == 1 ? 0.4f : 0), y = 0.9f - gy * 0.47f - (k == 2 ? 0.4f : 0);
        v[i].pos[0] = (int16_t) lrintf(x * 32767); v[i].pos[1] = (int16_t) lrintf(y * 32767);
        rgba(v[i].c0, i * 5, 255 - i * 5, (i * 11) & 255, 255 - i);
        v[i].c1 = (uint32_t) ((i * 21) % 1024) | (uint32_t) (1023 - i * 13) << 10 | (uint32_t) ((i * 7) % 1024) << 20 | (uint32_t) (i % 4) << 30;
        v[i].c2[0] = f2h(i / 24.0f); v[i].c2[1] = f2h(1.5f - i / 48.0f); v[i].c2[2] = f2h(0.25f + i / 96.0f); v[i].c2[3] = f2h(1);
        rgba(v[i].c3, i * 3, i * 5, 200 - i * 4, 128 + i);
        v[i].c4[0] = (int8_t) (-128 + i * 5); v[i].c4[1] = (int8_t) (127 - i * 5); v[i].c4[2] = (int8_t) (i * 2 - 40); v[i].c4[3] = (int8_t) (i - 128);
        v[i].c5[0] = (uint16_t) (i * 1300); v[i].c5[1] = (uint16_t) (65535 - i * 1000);
        rgba(v[i].c6, i * 5, i * 3, 255 - i * 5, i % 8);
        v[i].c7 = (uint32_t) (i * 20) | (uint32_t) (1000 - i * 20) << 10 | 512u << 20 | (uint32_t) (i % 4) << 30;
        v[i].c8[0] = i / 47.0f; v[i].c8[1] = 1 - i / 47.0f; v[i].c8[2] = 0.5f;
        v[i].c9[0] = (int16_t) (i * 40 - 900); v[i].c9[1] = (int16_t) (900 - i * 35);
        v[i].c10[0] = (uint8_t) (i * 5); v[i].c10[1] = 128;
        v[i].c11 = (uint32_t) i * 0x010305u + 0x102030u;
    }
    ID3DBlob *vb = NULL;
    ID3D11VertexShader *vs = mk_vs(SRC_VF, "vsv", &vb);
    D3D11_INPUT_ELEMENT_DESC il[] = {
        { "POSITION", 0, DXGI_FORMAT_R16G16_SNORM, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 4, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 1, DXGI_FORMAT_R10G10B10A2_UNORM, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 2, DXGI_FORMAT_R16G16B16A16_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 3, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 4, DXGI_FORMAT_R8G8B8A8_SNORM, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 5, DXGI_FORMAT_R16G16_UNORM, 0, 28, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 6, DXGI_FORMAT_R8G8B8A8_UINT, 0, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 7, DXGI_FORMAT_R10G10B10A2_UINT, 0, 36, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 8, DXGI_FORMAT_R32G32B32_FLOAT, 0, 40, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 9, DXGI_FORMAT_R16G16_SINT, 0, 52, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 10, DXGI_FORMAT_R8G8_UNORM, 0, 56, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "C", 11, DXGI_FORMAT_R32_UINT, 0, 60, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3D11InputLayout *lay = NULL;
    CK(ID3D11Device_CreateInputLayout(dev, il, 13, ID3D10Blob_GetBufferPointer(vb), ID3D10Blob_GetBufferSize(vb), &lay));
    ID3D11Buffer *vbuf = mk_buf(sizeof v, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0, v);
    fb f = mk_fb(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&f, 0, 0, 0, 1);
    ID3D11DeviceContext_IASetInputLayout(ctx, lay);
    UINT stride = sizeof(struct vf), off = 0;
    ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vbuf, &stride, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
    for (int k = 0; k < 12; k++) {
        float cb[16] = { 0, 0, 0, 0, 1, 1, 1, 1, 0.5f, 0.5f, 0, 0, (float) k, 0, 0, 0 };
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
        ID3D11DeviceContext_Draw(ctx, NV, 0);
    }
    // インスタンス：角は頂点ごと・ずらしは step 1（R16G16_FLOAT）・色は step 2・大きさは step 3
    ID3DBlob *jb = NULL;
    ID3D11VertexShader *vj = mk_vs(SRC_VF, "vsj", &jb);
    float corner[6][2] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 0, 1 }, { 1, 0 }, { 1, 1 } };
    uint16_t offs[16][2];
    uint8_t cols[16][4];
    float scl[16];
    for (int i = 0; i < 16; i++) {
        offs[i][0] = f2h(2.0f + i * 15.0f); offs[i][1] = f2h(194.0f);
        rgba(cols[i], (i * 67) & 255, (i * 131 + 60) & 255, 255 - i * 15, 255);
        scl[i] = 1.0f - i * 0.05f;
    }
    ID3D11Buffer *b0 = mk_buf(sizeof corner, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0, corner);
    ID3D11Buffer *b1 = mk_buf(sizeof offs, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0, offs);
    ID3D11Buffer *b2 = mk_buf(sizeof cols, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0, cols);
    ID3D11Buffer *b3 = mk_buf(sizeof scl, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0, scl);
    uint16_t ix[6] = { 0, 1, 2, 3, 4, 5 };
    ID3D11Buffer *ib = mk_buf(sizeof ix, D3D11_USAGE_IMMUTABLE, D3D11_BIND_INDEX_BUFFER, 0, 0, 0, ix);
    for (int pass = 0; pass < 2; pass++) {
        D3D11_INPUT_ELEMENT_DESC jl[] = {
            { "CORNER", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "OFF", 0, DXGI_FORMAT_R16G16_FLOAT, 1, 0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "ICOL", 0, DXGI_FORMAT_R8G8B8A8_UNORM, 2, 0, D3D11_INPUT_PER_INSTANCE_DATA, pass ? 0u : 2u },
            { "ISC", 0, DXGI_FORMAT_R32_FLOAT, 3, 0, D3D11_INPUT_PER_INSTANCE_DATA, 3 },
        };
        ID3D11InputLayout *jlay = NULL;
        HRESULT hr = ID3D11Device_CreateInputLayout(dev, jl, 4, ID3D10Blob_GetBufferPointer(jb), ID3D10Blob_GetBufferSize(jb), &jlay);
        if (FAILED(hr)) { val("step rate %s の頂点の並びを作れない 0x%08lx", pass ? "0" : "2・3", (unsigned long) hr); continue; }
        ID3D11DeviceContext_IASetInputLayout(ctx, jlay);
        ID3D11Buffer *bs[4] = { b0, b1, b2, b3 };
        UINT st[4] = { 8, 4, 4, 4 }, of[4] = { 0 };
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 4, bs, st, of);
        ID3D11DeviceContext_VSSetShader(ctx, vj, NULL, 0);
        float cb[16] = { 0, 0, 0, 0, 1, 1, 1, 1, 0.5f, 0.5f, 0, 0, 0, 0, 0, 0 };
        if (!pass) {
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
            ID3D11DeviceContext_DrawInstanced(ctx, 6, 16, 0, 0);
            cb[13] = 15;
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
            ID3D11DeviceContext_DrawInstanced(ctx, 6, 8, 0, 5);
            cb[13] = 30;
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
            ID3D11DeviceContext_IASetIndexBuffer(ctx, ib, DXGI_FORMAT_R16_UINT, 0);
            ID3D11DeviceContext_DrawIndexedInstanced(ctx, 6, 6, 0, 0, 3);
        } else {
            cb[13] = 45;
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) g_cbq, 0, NULL, cb, 0, 0);
            ID3D11DeviceContext_DrawInstanced(ctx, 6, 6, 0, 2);
        }
    }
    img m = show(f.ct, 0, 0, 0, 0, "頂点の形式 12 種類とインスタンス");
    for (int k = 0; k < 12; k++) val_px(&m, (k % 4) * 64 + 10, (k / 4) * 64 + 10, "頂点の形式");
    for (int r = 0; r < 4; r++)
        for (int i = 0; i < 16; i += 3) val_px(&m, 2 + i * 15 + 3, 194 + r * 15 + 3, "インスタンス");
}

// ---- 15. 遅延コンテキスト（Deferred context）----
static const char *SRC_DC =
    "RWTexture2D<unorm float4> u : register(u0);\n"
    "[numthreads(8, 8, 1)] void cs(uint3 d : SV_DispatchThreadID) { u[d.xy] = float4(d.x / 31.0, 0.2, d.y / 31.0, 1); }\n";
static void scene_deferred(void) {
    ID3D11DeviceContext *dc = NULL;
    CK(ID3D11Device_CreateDeferredContext(dev, 0, &dc));
    q_init();
    fb f = mk_fb(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    ID3D11Buffer *dyn = mk_buf(64, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0, NULL);
    ID3D11Texture2D *U = mk_tex(32, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE, 0, NULL);
    ID3D11UnorderedAccessView *uu = mk_uav(U, NULL);
    ID3D11ComputeShader *cs = mk_cs(SRC_DC, "cs");
    g_tw = g_th = 128;
    // 記録 1：消す・Map DISCARD の定数で 8 つ描く・UpdateSubresource の定数で 1 つ・計算で U に書く
    float c[4] = { 0.1f, 0.1f, 0.3f, 1 };
    ID3D11DeviceContext_ClearRenderTargetView(dc, f.rtv, c);
    ID3D11DeviceContext_OMSetRenderTargets(dc, 1, &f.rtv, NULL);
    D3D11_VIEWPORT vp = { 0, 0, 128, 128, 0, 1 };
    ID3D11DeviceContext_RSSetViewports(dc, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(dc, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(dc, g_vsq, NULL, 0);
    ID3D11DeviceContext_PSSetShader(dc, g_psq, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(dc, 0, 1, &dyn);
    ID3D11DeviceContext_PSSetConstantBuffers(dc, 0, 1, &dyn);
    for (int k = 0; k < 8; k++) {
        D3D11_MAPPED_SUBRESOURCE m;
        CK(ID3D11DeviceContext_Map(dc, (ID3D11Resource *) dyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
        float cb[16] = { nx(4 + k * 15.0f), ny(4), nx(16 + k * 15.0f), ny(60), 0.3f + k * 0.09f, 0.9f - k * 0.1f, 0.4f, 1, 0.5f, 0.5f, 0, 0, 0, 0, 0, 0 };
        memcpy(m.pData, cb, sizeof cb);
        ID3D11DeviceContext_Unmap(dc, (ID3D11Resource *) dyn, 0);
        ID3D11DeviceContext_Draw(dc, 4, 0);
    }
    float cb2[16] = { nx(4), ny(64), nx(60), ny(124), 1, 0.6f, 0.1f, 1, 0.5f, 0.5f, 0, 0, 2, 0, 0, 0 };
    ID3D11DeviceContext_UpdateSubresource(dc, (ID3D11Resource *) g_cbq, 0, NULL, cb2, 0, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(dc, 0, 1, &g_cbq);
    ID3D11DeviceContext_PSSetConstantBuffers(dc, 0, 1, &g_cbq);
    ID3D11DeviceContext_Draw(dc, 4, 0);
    ID3D11DeviceContext_CSSetShader(dc, cs, NULL, 0);
    ID3D11DeviceContext_CSSetUnorderedAccessViews(dc, 0, 1, &uu, NULL);
    ID3D11DeviceContext_Dispatch(dc, 4, 4, 1);
    ID3D11CommandList *l1 = NULL, *l2 = NULL;
    CK(ID3D11DeviceContext_FinishCommandList(dc, FALSE, &l1));
    // 記録 2：U を的へ写し、加算で 1 つ描く
    D3D11_BOX box = { 0, 0, 0, 32, 32, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(dc, (ID3D11Resource *) f.ct, 0, 80, 80, 0, (ID3D11Resource *) U, 0, &box);
    ID3D11DeviceContext_OMSetRenderTargets(dc, 1, &f.rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(dc, 1, &vp);
    ID3D11DeviceContext_IASetPrimitiveTopology(dc, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(dc, g_vsq, NULL, 0);
    ID3D11DeviceContext_PSSetShader(dc, g_psq, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(dc, 0, 1, &dyn);
    ID3D11DeviceContext_PSSetConstantBuffers(dc, 0, 1, &dyn);
    float bf[4] = { 0 };
    ID3D11DeviceContext_OMSetBlendState(dc, mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, 15), bf, 0xffffffff);
    D3D11_MAPPED_SUBRESOURCE m;
    CK(ID3D11DeviceContext_Map(dc, (ID3D11Resource *) dyn, 0, D3D11_MAP_WRITE_DISCARD, 0, &m));
    float cb3[16] = { nx(70), ny(64), nx(124), ny(76), 0.2f, 0.2f, 0.6f, 1, 0.5f, 0.5f, 0, 0, 0, 0, 0, 0 };
    memcpy(m.pData, cb3, sizeof cb3);
    ID3D11DeviceContext_Unmap(dc, (ID3D11Resource *) dyn, 0);
    ID3D11DeviceContext_Draw(dc, 4, 0);
    CK(ID3D11DeviceContext_FinishCommandList(dc, FALSE, &l2));
    // すぐのコンテキストで、関係のない状態を置いてから流す。1 は 2 回
    set_bs(mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, 0, 0, 0, 15), NULL);
    ID3D11DeviceContext_ExecuteCommandList(ctx, l1, FALSE);
    ID3D11DeviceContext_ExecuteCommandList(ctx, l1, FALSE);
    ID3D11DeviceContext_ExecuteCommandList(ctx, l2, FALSE);
    // すぐのコンテキストで、もう 1 つ描く（状態は消えているので付け直す）
    targets(1, &f.rtv, NULL, 128, 128);
    q_use(NULL);
    quad(70, 100, 124, 124, 0.9f, 0.9f, 0.9f, 1, 0.5f, 0.5f, 2);
    img mf = show(f.ct, 0, 0, 0, 0, "遅延コンテキスト");
    show(U, 0, 0, 0, 0, "遅延コンテキストの計算");
    val_px(&mf, 10, 30, "遅延");
    val_px(&mf, 115, 30, "遅延");
    val_px(&mf, 30, 100, "遅延");
    val_px(&mf, 90, 90, "遅延");
    val_px(&mf, 100, 70, "遅延");
    val_px(&mf, 100, 110, "遅延");
}

// ---- stream output：動かないことの確認 ----
static const char *SRC_SO =
    "struct VO { float4 p : SV_Position; float4 c : COLOR; };\n"
    "VO vs(uint id : SV_VertexID) { VO o; o.p = float4(id * 0.1, id * 0.2, 0.5, 1); o.c = float4(id * 0.5, 1, 2, 3); return o; }\n";
static void scene_so(void) {
    ID3DBlob *b = NULL;
    mk_vs(SRC_SO, "vs", &b);
    D3D11_SO_DECLARATION_ENTRY de[2] = { { 0, "SV_Position", 0, 0, 4, 0 }, { 0, "COLOR", 0, 0, 4, 0 } };
    UINT stride = 32;
    ID3D11GeometryShader *gs = NULL;
    HRESULT hr = ID3D11Device_CreateGeometryShaderWithStreamOutput(dev, ID3D10Blob_GetBufferPointer(b), ID3D10Blob_GetBufferSize(b), de, 2, &stride, 1,
                                                                    D3D11_SO_NO_RASTERIZED_STREAM, NULL, &gs);
    val("CreateGeometryShaderWithStreamOutput 0x%08lx", (unsigned long) hr);
    if (FAILED(hr)) return;
    ID3D11Buffer *sob = mk_buf(32 * 16, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT, 0, 0, 0, NULL);
    D3D11_QUERY_DESC qd = { D3D11_QUERY_SO_STATISTICS, 0 };
    ID3D11Query *q = NULL;
    CK(ID3D11Device_CreateQuery(dev, &qd, &q));
    UINT off = 0;
    ID3D11DeviceContext_SOSetTargets(ctx, 1, &sob, &off);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
    ID3D11DeviceContext_VSSetShader(ctx, mk_vs(SRC_SO, "vs", NULL), NULL, 0);
    ID3D11DeviceContext_GSSetShader(ctx, gs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) q);
    ID3D11DeviceContext_Draw(ctx, 6, 0);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) q);
    ID3D11Buffer *none = NULL;
    ID3D11DeviceContext_SOSetTargets(ctx, 1, &none, &off);
    D3D11_QUERY_DATA_SO_STATISTICS s = { 0 };
    int r = get_data(q, &s, sizeof s);
    val("SO_STATISTICS：返った %d・書いた %llu・要った %llu", r, (unsigned long long) s.NumPrimitivesWritten, (unsigned long long) s.PrimitivesStorageNeeded);
    float *o = read_buf(sob, 32 * 16);
    for (int i = 0; i < 6; i++) val("SO の %d：%.2f %.2f %.2f %.2f / %.2f %.2f %.2f %.2f", i, o[i * 8], o[i * 8 + 1], o[i * 8 + 2], o[i * 8 + 3], o[i * 8 + 4], o[i * 8 + 5], o[i * 8 + 6], o[i * 8 + 7]);
}

// ---- tessellation：動かないことの確認 ----
static const char *SRC_TS =
    "struct VO { float3 p : POS; };\n"
    "VO vs(uint id : SV_VertexID) { VO o; o.p = id == 0 ? float3(-0.9, -0.9, 0.5) : id == 1 ? float3(0, 0.9, 0.5) : float3(0.9, -0.9, 0.5); return o; }\n"
    "struct HC { float e[3] : SV_TessFactor; float i : SV_InsideTessFactor; };\n"
    "HC hc(InputPatch<VO, 3> ip) { HC o; o.e[0] = o.e[1] = o.e[2] = 4; o.i = 4; return o; }\n"
    "[domain(\"tri\")] [partitioning(\"integer\")] [outputtopology(\"triangle_cw\")] [outputcontrolpoints(3)] [patchconstantfunc(\"hc\")]\n"
    "VO hs(InputPatch<VO, 3> ip, uint i : SV_OutputControlPointID) { return ip[i]; }\n"
    "struct DO { float4 pos : SV_Position; float4 col : COLOR; };\n"
    "[domain(\"tri\")] DO ds(HC c, float3 b : SV_DomainLocation, const OutputPatch<VO, 3> p) { DO o; float3 q = p[0].p * b.x + p[1].p * b.y + p[2].p * b.z;\n"
    "  o.pos = float4(q, 1); o.col = float4(b, 1); return o; }\n"
    "float4 ps(DO i) : SV_Target { return i.col; }\n";
static void scene_tess(void) {
    ID3DBlob *hb = compile(SRC_TS, "hs", "hs_5_0"), *db = compile(SRC_TS, "ds", "ds_5_0");
    ID3D11HullShader *hs = NULL;
    ID3D11DomainShader *ds = NULL;
    HRESULT h1 = ID3D11Device_CreateHullShader(dev, ID3D10Blob_GetBufferPointer(hb), ID3D10Blob_GetBufferSize(hb), NULL, &hs);
    HRESULT h2 = ID3D11Device_CreateDomainShader(dev, ID3D10Blob_GetBufferPointer(db), ID3D10Blob_GetBufferSize(db), NULL, &ds);
    val("CreateHullShader 0x%08lx・CreateDomainShader 0x%08lx", (unsigned long) h1, (unsigned long) h2);
    if (FAILED(h1) || FAILED(h2)) return;
    fb f = mk_fb(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&f, 0, 0, 0, 1);
    ID3D11DeviceContext_RSSetState(ctx, mk_rs(D3D11_FILL_WIREFRAME, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE));
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST);
    ID3D11DeviceContext_VSSetShader(ctx, mk_vs(SRC_TS, "vs", NULL), NULL, 0);
    ID3D11DeviceContext_HSSetShader(ctx, hs, NULL, 0);
    ID3D11DeviceContext_DSSetShader(ctx, ds, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, mk_ps(SRC_TS, "ps"), NULL, 0);
    D3D11_QUERY_DESC qs = { D3D11_QUERY_PIPELINE_STATISTICS, 0 };
    ID3D11Query *q = NULL;
    CK(ID3D11Device_CreateQuery(dev, &qs, &q));
    ID3D11DeviceContext_Begin(ctx, (ID3D11Asynchronous *) q);
    ID3D11DeviceContext_Draw(ctx, 3, 0);
    ID3D11DeviceContext_End(ctx, (ID3D11Asynchronous *) q);
    ID3D11DeviceContext_HSSetShader(ctx, NULL, NULL, 0);
    ID3D11DeviceContext_DSSetShader(ctx, NULL, NULL, 0);
    D3D11_QUERY_DATA_PIPELINE_STATISTICS s = { 0 };
    int r = get_data(q, &s, sizeof s);
    val("tessellation の statistics：返った %d・HSInvocations %llu・DSInvocations %llu・CPrimitives %llu", r, (unsigned long long) s.HSInvocations, (unsigned long long) s.DSInvocations,
        (unsigned long long) s.CPrimitives);
    img m = show(f.ct, 0, 0, 0, 0, "tessellation");
    int lit = 0;
    for (int i = 0; i < m.w * m.h; i++) if (m.px[i * 4] | m.px[i * 4 + 1] | m.px[i * 4 + 2]) lit++;
    val("tessellation で塗った画素 %d", lit);
}
