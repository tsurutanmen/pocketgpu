// テクスチャまわりの場面：配列・cube・mip・サンプラー・影・sRGB・形式・写し・GenerateMips

static void rgba(uint8_t *p, int r, int g, int b, int a) { p[0] = (uint8_t) r; p[1] = (uint8_t) g; p[2] = (uint8_t) b; p[3] = (uint8_t) a; }

// ---- 5. テクスチャ配列と 3D テクスチャ ----
static const char *SRC_TA = Q_HEAD
    "Texture2DArray ta : register(t0); Texture3D t3 : register(t1); SamplerState sp : register(s0); SamplerState sl : register(s1);\n"
    "float4 psa(VO i) : SV_Target { if (ex.x == 7) return t3.Sample(sl, float3(i.uv, ex.z));\n"
    "  return ta.SampleLevel(sp, float3(i.uv, ex.y), 0); }\n";
static void scene_texarr(void) {
    enum { S = 32, N = 6 };
    uint8_t *px = malloc(S * S * 4 * N);
    D3D11_SUBRESOURCE_DATA init[N];
    for (int s = 0; s < N; s++) {
        int ck = (s + 1) * 2;
        for (int y = 0; y < S; y++)
            for (int x = 0; x < S; x++) {
                int on = ((x / ck) + (y / ck)) & 1;
                rgba(px + ((size_t) s * S * S + y * S + x) * 4, on ? 40 * s + 30 : 20, on ? 255 - 40 * s : 40, on ? (s * 97) & 255 : 60 + x * 4, 255);
            }
        init[s] = (D3D11_SUBRESOURCE_DATA) { px + (size_t) s * S * S * 4, S * 4, 0 };
    }
    ID3D11Texture2D *ta = mk_tex(S, S, 1, N, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, 0, init);
    ID3D11ShaderResourceView *all = mk_srv(ta, NULL);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY,
                                           .Texture2DArray = { 0, 1, 2, 3 } };
    ID3D11ShaderResourceView *part = mk_srv(ta, &sd);
    // 3D：8x8x8 の色の立方体
    uint8_t v3[8 * 8 * 8 * 4];
    for (int z = 0; z < 8; z++)
        for (int y = 0; y < 8; y++)
            for (int x = 0; x < 8; x++) rgba(v3 + ((z * 8 + y) * 8 + x) * 4, x * 36, y * 36, z * 36, 255);
    D3D11_TEXTURE3D_DESC d3 = { 8, 8, 8, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA i3 = { v3, 32, 256 };
    ID3D11Texture3D *t3 = NULL;
    CK(ID3D11Device_CreateTexture3D(dev, &d3, &i3, &t3));
    ID3D11ShaderResourceView *s3 = mk_srv(t3, NULL);
    fb f = mk_fb(256, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&f, 0, 0, 0, 1);
    q_use(mk_ps(SRC_TA, "psa"));
    ID3D11SamplerState *ss[2] = { mk_samp(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP, NULL, 0),
                                  mk_samp(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, NULL, 0) };
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, ss);
    ID3D11ShaderResourceView *sv[2] = { all, s3 };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, sv);
    for (int s = 0; s < N; s++) quad4((float) (s % 4) * 64, (float) (s / 4) * 64, (float) (s % 4) * 64 + 64, (float) (s / 4) * 64 + 64, 1, 1, 1, 1, 0.5f, 0.5f, 0, (float) s, 0, 0);
    // 層 2 から 3 枚の view の 1 番 → 層 3
    sv[0] = part;
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, sv);
    quad4(128, 64, 192, 128, 1, 1, 1, 1, 0.5f, 0.5f, 0, 1, 0, 0);
    quad4(192, 64, 256, 128, 1, 1, 1, 1, 0.5f, 0.5f, 7, 0, 0.3f, 0);
    img m = show(f.ct, 0, 0, 0, 0, "テクスチャ配列と 3D");
    val_px(&m, 160, 100, "view の一部");
    val_px(&m, 224, 96, "3D");
    val_px(&m, 200, 70, "3D");
}

// ---- 5. cube map（cube の配列と、面に描いてから読む）----
static const char *SRC_CU = Q_HEAD
    "TextureCube tc : register(t0); TextureCubeArray tca : register(t1); SamplerState sl : register(s0);\n"
    "float4 psc(VO i) : SV_Target { float lon = (i.uv.x * 2 - 1) * 3.14159265, lat = (0.5 - i.uv.y) * 3.14159265;\n"
    "  float3 d = float3(cos(lat) * sin(lon), sin(lat), cos(lat) * cos(lon));\n"
    "  if (ex.x == 1) return tca.Sample(sl, float4(d, ex.y)); return tc.Sample(sl, d); }\n";
static void scene_cube(void) {
    enum { S = 16 };
    uint8_t px[2][6][S * S * 4];
    D3D11_SUBRESOURCE_DATA init[12];
    for (int c = 0; c < 2; c++)
        for (int f = 0; f < 6; f++) {
            for (int y = 0; y < S; y++)
                for (int x = 0; x < S; x++) {
                    int base[6][3] = { { 255, 60, 60 }, { 60, 255, 60 }, { 60, 60, 255 }, { 255, 255, 60 }, { 255, 60, 255 }, { 60, 255, 255 } };
                    int k = (x == 0 || y == 0) ? 0 : 1; // 面の端に暗い線
                    int r = base[f][0] * k * (c ? 1 : x + 8) / (c ? 2 : 24), g = base[f][1] * k * (c ? 1 : y + 8) / (c ? 2 : 24), b = base[f][2] * k / (c ? 3 : 1);
                    rgba(px[c][f] + (y * S + x) * 4, r > 255 ? 255 : r, g > 255 ? 255 : g, b, 255);
                }
            init[c * 6 + f] = (D3D11_SUBRESOURCE_DATA) { px[c][f], S * 4, 0 };
        }
    ID3D11Texture2D *cube = mk_tex(S, S, 1, 6, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, D3D11_RESOURCE_MISC_TEXTURECUBE, init);
    ID3D11Texture2D *carr = mk_tex(S, S, 1, 12, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, D3D11_RESOURCE_MISC_TEXTURECUBE, init);
    ID3D11ShaderResourceView *sc = mk_srv(cube, NULL);
    D3D11_SHADER_RESOURCE_VIEW_DESC ad = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBEARRAY, .TextureCubeArray = { 0, 1, 0, 2 } };
    ID3D11ShaderResourceView *sca = mk_srv(carr, &ad);
    ID3D11SamplerState *sl = mk_samp(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, NULL, 0);
    ID3D11PixelShader *ps = mk_ps(SRC_CU, "psc");
    fb f = mk_fb(256, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    for (int pass = 0; pass < 3; pass++) {
        if (pass == 1) {
            // 面 4（+Z）に描く
            D3D11_RENDER_TARGET_VIEW_DESC rd = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY, .Texture2DArray = { 0, 4, 1 } };
            ID3D11RenderTargetView *fv = mk_rtv(cube, &rd);
            targets(1, &fv, NULL, S, S);
            q_use(NULL);
            quad(2, 2, 14, 14, 1, 1, 1, 1, 0.5f, 0.5f, 4);
        }
        fb_begin(&f, 0, 0, 0, 1);
        q_use(ps);
        ID3D11ShaderResourceView *sv[2] = { sc, sca };
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, sv);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &sl);
        quad4(0, 0, 256, 128, 1, 1, 1, 1, 0.5f, 0.5f, pass == 2 ? 1.f : 0.f, 1, 0, 0);
        ID3D11ShaderResourceView *nn[2] = { NULL, NULL };
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 2, nn);
        img m = show(f.ct, 0, 0, 0, 0, pass == 0 ? "cube" : pass == 1 ? "面 4 に描いた cube" : "cube の配列の 1 番");
        val_px(&m, 128, 64, "cube");
        val_px(&m, 64, 64, "cube");
        val_px(&m, 128, 5, "cube");
    }
}

// ---- 5. mip の選び方 ----
static const char *SRC_MIP = Q_HEAD
    "Texture2D tx : register(t0); SamplerState sm : register(s0); SamplerState sp : register(s1);\n"
    "VO vsf(uint id : SV_VertexID) { float2 t = float2(id & 1, id >> 1); VO o; float w = lerp(8, 1, t.y);\n"
    "  float2 p = lerp(float2(lerp(-0.15, 0.15, t.x), 0.9), float2(lerp(-1, 1, t.x), -1), t.y);\n"
    "  o.pos = float4(p * w, 0.5 * w, w); o.col = 1; o.uv = float2(t.x * 4, t.y * 16); return o; }\n"
    "float4 psf(VO i) : SV_Target { return tx.Sample(sm, i.uv); }\n"
    "float4 pse(VO i) : SV_Target { float k = floor(i.uv.x * 7); float2 uv = i.uv * 2;\n"
    "  if (i.uv.y < 0.25) return tx.SampleLevel(sm, uv, k);\n"
    "  if (i.uv.y < 0.5) return tx.SampleBias(sm, uv, k - 1);\n"
    "  if (i.uv.y < 0.75) return tx.SampleGrad(sm, uv, float2(exp2(k) / 64, 0), float2(0, exp2(k) / 64));\n"
    "  return tx.Load(int3((int2(i.pos.xy) % 64) >> (int) k, (int) k)); }\n"
    "float4 psv(VO i) : SV_Target { return tx.SampleLevel(sp, i.uv, 0); }\n";
static void scene_mip(void) {
    enum { S = 64, M = 7 };
    static const int col[M][3] = { { 255, 40, 40 }, { 40, 255, 40 }, { 40, 40, 255 }, { 255, 255, 40 }, { 255, 40, 255 }, { 40, 255, 255 }, { 255, 255, 255 } };
    uint8_t *px = malloc(S * S * 4 * 2);
    D3D11_SUBRESOURCE_DATA init[M];
    size_t off = 0;
    for (int m = 0; m < M; m++) {
        int s = S >> m;
        for (int y = 0; y < s; y++)
            for (int x = 0; x < s; x++) {
                int k = ((x / 2) + (y / 2)) & 1 ? 1 : 2;
                rgba(px + off + (size_t) (y * s + x) * 4, col[m][0] * k / 2, col[m][1] * k / 2, col[m][2] * k / 2, 255);
            }
        init[m] = (D3D11_SUBRESOURCE_DATA) { px + off, (UINT) s * 4, 0 };
        off += (size_t) s * s * 4;
    }
    ID3D11Texture2D *t = mk_tex(S, S, M, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, 0, init);
    ID3D11ShaderResourceView *sv = mk_srv(t, NULL);
    ID3DBlob *b = NULL;
    ID3D11VertexShader *vsf = mk_vs(SRC_MIP, "vsf", &b);
    ID3D11PixelShader *psf = mk_ps(SRC_MIP, "psf"), *pse = mk_ps(SRC_MIP, "pse"), *psv = mk_ps(SRC_MIP, "psv");
    fb f = mk_fb(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    // 床：サンプラーを変えて 5 枚
    const char *nm[5] = { "trilinear", "point", "異方性 16", "LOD バイアス +1.5", "MinLOD 2 MaxLOD 4" };
    for (int k = 0; k < 5; k++) {
        D3D11_SAMPLER_DESC sd = { .Filter = k == 1 ? D3D11_FILTER_MIN_MAG_MIP_POINT : k == 2 ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                                  .AddressU = D3D11_TEXTURE_ADDRESS_WRAP, .AddressV = D3D11_TEXTURE_ADDRESS_WRAP, .AddressW = D3D11_TEXTURE_ADDRESS_WRAP,
                                  .MipLODBias = k == 3 ? 1.5f : 0, .MaxAnisotropy = 16, .ComparisonFunc = D3D11_COMPARISON_NEVER,
                                  .MinLOD = k == 4 ? 2.0f : 0, .MaxLOD = k == 4 ? 4.0f : D3D11_FLOAT32_MAX };
        ID3D11SamplerState *s = NULL;
        CK(ID3D11Device_CreateSamplerState(dev, &sd, &s));
        fb_begin(&f, 0, 0, 0, 1);
        q_use(psf);
        ID3D11DeviceContext_VSSetShader(ctx, vsf, NULL, 0);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &sv);
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s);
        ID3D11DeviceContext_Draw(ctx, 4, 0);
        img m = show(f.ct, 0, 0, 0, 0, nm[k]);
        val_px(&m, 64, 120, nm[k]);
        val_px(&m, 64, 60, nm[k]);
        val_px(&m, 64, 20, nm[k]);
    }
    // 明示の選び方：SampleLevel・SampleBias・SampleGrad・Load（左から mip 0〜6）
    ID3D11SamplerState *sl = mk_samp(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_WRAP, NULL, 0);
    ID3D11SamplerState *sp = mk_samp(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_WRAP, NULL, 0);
    fb g = mk_fb(224, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&g, 0, 0, 0, 1);
    q_use(pse);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &sv);
    ID3D11SamplerState *ss[2] = { sl, sp };
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, ss);
    quad(0, 0, 224, 128, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    img me = show(g.ct, 0, 0, 0, 0, "SampleLevel・Bias・Grad・Load");
    for (int k = 0; k < 7; k++)
        for (int r = 0; r < 4; r++) val_px(&me, k * 32 + 16, r * 32 + 16, "明示");
    // mip 2 から 3 段の view（MostDetailedMip）
    D3D11_SHADER_RESOURCE_VIEW_DESC vd = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { 2, 3 } };
    ID3D11ShaderResourceView *part = mk_srv(t, &vd);
    fb_begin(&f, 0, 0, 0, 1);
    q_use(psv);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &part);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, ss);
    quad(0, 0, 128, 128, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    img mv = show(f.ct, 0, 0, 0, 0, "mip 2 からの view");
    val_px(&mv, 64, 64, "mip 2 からの view");
}

// ---- 5. サンプラーの種類：繰り返し方 5 つ x 点と線形・ボーダー色・Gather・型つきバッファ・バイトのバッファ・構造化バッファ ----
static const char *SRC_SA = Q_HEAD
    "Texture2D tx : register(t0); Buffer<float4> tb : register(t1); ByteAddressBuffer bb : register(t2); StructuredBuffer<float3> sb : register(t3);\n"
    "SamplerState s0 : register(s0);\n"
    "float4 pss(VO i) : SV_Target { float2 uv = i.uv * 3 - 1;\n"
    "  if (ex.x == 1) { float4 g = tx.GatherRed(s0, i.uv); return float4(g.x, g.y, g.z * 0.5 + g.w * 0.5, 1); }\n"
    "  if (ex.x == 2) { uint k = (uint) (i.uv.x * 8) + 8 * (uint) (i.uv.y * 8); return tb.Load(k); }\n"
    "  if (ex.x == 3) { uint k = (uint) (i.uv.x * 8) + 8 * (uint) (i.uv.y * 8); uint v = bb.Load(k * 4);\n"
    "    return float4(v & 255, (v >> 8) & 255, (v >> 16) & 255, 255) / 255; }\n"
    "  if (ex.x == 4) { uint k = (uint) (i.uv.x * 8) + 8 * (uint) (i.uv.y * 8); return float4(sb[k], 1); }\n"
    "  return tx.Sample(s0, uv); }\n";
static void scene_sampler(void) {
    uint8_t px[4 * 4 * 4];
    for (int i = 0; i < 16; i++) rgba(px + i * 4, (i * 53) & 255, (i * 97 + 40) & 255, (i * 29 + 100) & 255, 128 + i * 8);
    D3D11_SUBRESOURCE_DATA init = { px, 16, 0 };
    ID3D11Texture2D *t = mk_tex(4, 4, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, 0, &init);
    ID3D11ShaderResourceView *sv = mk_srv(t, NULL);
    // 型つきバッファ（RGBA8）・バイトのバッファ・構造化バッファ（float3）
    uint8_t tbd[64 * 4];
    uint32_t bbd[64];
    float sbd[64 * 3];
    for (int i = 0; i < 64; i++) {
        rgba(tbd + i * 4, i * 4, 255 - i * 4, (i * 37) & 255, 255);
        bbd[i] = (uint32_t) ((i * 3) | ((i * 7 & 255) << 8) | ((200 - i) << 16));
        sbd[i * 3] = i / 63.0f; sbd[i * 3 + 1] = 0.5f; sbd[i * 3 + 2] = 1 - i / 63.0f;
    }
    ID3D11Buffer *tb = mk_buf(sizeof tbd, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0, tbd);
    D3D11_SHADER_RESOURCE_VIEW_DESC td = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D11_SRV_DIMENSION_BUFFER, .Buffer = { { 0 }, { 64 } } };
    ID3D11ShaderResourceView *tbv = mk_srv(tb, &td);
    ID3D11Buffer *bb = mk_buf(sizeof bbd, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0, bbd);
    D3D11_SHADER_RESOURCE_VIEW_DESC bd = { .Format = DXGI_FORMAT_R32_TYPELESS, .ViewDimension = D3D11_SRV_DIMENSION_BUFFEREX, .BufferEx = { 0, 64, D3D11_BUFFEREX_SRV_FLAG_RAW } };
    ID3D11ShaderResourceView *bbv = mk_srv(bb, &bd);
    ID3D11Buffer *sb = mk_buf(sizeof sbd, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 12, sbd);
    ID3D11ShaderResourceView *sbv = mk_srv(sb, NULL);
    fb f = mk_fb(320, 192, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&f, 0.1f, 0.1f, 0.1f, 1);
    q_use(mk_ps(SRC_SA, "pss"));
    ID3D11ShaderResourceView *svs[4] = { sv, tbv, bbv, sbv };
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 4, svs);
    static const D3D11_TEXTURE_ADDRESS_MODE am[5] = { D3D11_TEXTURE_ADDRESS_WRAP, D3D11_TEXTURE_ADDRESS_MIRROR, D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_BORDER, D3D11_TEXTURE_ADDRESS_MIRROR_ONCE };
    float magenta[4] = { 1, 0, 1, 1 }, white[4] = { 1, 1, 1, 1 }, clear0[4] = { 0, 0, 0, 0 }, odd[4] = { 0.25f, 0.5f, 0.75f, 0.5f };
    for (int row = 0; row < 2; row++)
        for (int c = 0; c < 5; c++) {
            ID3D11SamplerState *s = mk_samp(row ? D3D11_FILTER_MIN_MAG_MIP_LINEAR : D3D11_FILTER_MIN_MAG_MIP_POINT, am[c], row ? white : magenta, 0);
            ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s);
            quad((float) c * 64, (float) row * 64, (float) c * 64 + 64, (float) row * 64 + 64, 1, 1, 1, 1, 0.5f, 0.5f, 0);
        }
    // 3 段目：ボーダー（透明の黒）・ボーダー（半端な色）・Gather・型つき・バイト・構造化
    ID3D11SamplerState *s0 = mk_samp(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_BORDER, clear0, 0);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s0);
    quad(0, 128, 53, 192, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    ID3D11SamplerState *s1 = mk_samp(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_BORDER, odd, 0);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &s1);
    quad(53, 128, 106, 192, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    ID3D11SamplerState *sp = mk_samp(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, NULL, 0);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &sp);
    for (int k = 1; k <= 4; k++) quad(106 + (k - 1) * 53.5f, 128, 106 + k * 53.5f, 192, 1, 1, 1, 1, 0.5f, 0.5f, (float) k);
    img m = show(f.ct, 0, 0, 0, 1, "サンプラー");
    // ボーダーの色が出る所（uv が 1 より外）と、各区画の真ん中
    for (int c = 0; c < 5; c++) {
        val_px(&m, c * 64 + 4, 4, "外側 点");
        val_px(&m, c * 64 + 4, 64 + 4, "外側 線形");
    }
    val_px(&m, 4, 132, "ボーダー 透明");
    val_px(&m, 57, 132, "ボーダー 半端な色");
    for (int k = 0; k < 4; k++) val_px(&m, 106 + k * 53 + 20, 150, "Gather・型つき・バイト・構造化");
}

// ---- 5. 比較サンプラーで影 ----
static const char *SRC_SH = Q_HEAD
    "Texture2D<float> sm : register(t0); SamplerComparisonState sc : register(s0); SamplerState sp : register(s1);\n"
    "float4 psh(VO i) : SV_Target { float ref = 0.25 + 0.5 * i.uv.x;\n"
    "  if (ex.x == 1) { float d = sm.Sample(sp, i.uv); return float4(d, d, d, 1); }\n"
    "  if (ex.x == 2) { float s = 0; for (int y = -1; y <= 1; y++) for (int x = -1; x <= 1; x++) s += sm.SampleCmpLevelZero(sc, i.uv, ref, int2(x, y)); s /= 9; return float4(s, s * 0.8, s * 0.6, 1); }\n"
    "  float s = sm.SampleCmpLevelZero(sc, i.uv, ref); return float4(s, s, s, 1); }\n";
static void scene_shadow(void) {
    static const DXGI_FORMAT tf[3] = { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R16_TYPELESS };
    static const DXGI_FORMAT df[3] = { DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D16_UNORM };
    static const DXGI_FORMAT sf[3] = { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS, DXGI_FORMAT_R16_UNORM };
    ID3D11ShaderResourceView *srv[3];
    for (int k = 0; k < 3; k++) {
        ID3D11Texture2D *t = mk_tex(128, 128, 1, 1, tf[k], 1, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, NULL);
        D3D11_DEPTH_STENCIL_VIEW_DESC dd = { .Format = df[k], .ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D };
        ID3D11DepthStencilView *dsv = mk_dsv(t, &dd);
        D3D11_SHADER_RESOURCE_VIEW_DESC sd = { .Format = sf[k], .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { 0, 1 } };
        srv[k] = mk_srv(t, &sd);
        ID3D11DeviceContext_ClearDepthStencilView(ctx, dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
        targets(0, NULL, dsv, 128, 128);
        q_use(NULL);
        set_ds(mk_dss(TRUE, TRUE, D3D11_COMPARISON_LESS, 0, 0, 0xff, 0xff), 0);
        quad(16, 16, 64, 64, 1, 1, 1, 1, 0.3f, 0.3f, 0);
        quad(48, 72, 112, 112, 1, 1, 1, 1, 0.6f, 0.6f, 0);
        tri(70, 10, 0.2f, 120, 10, 0.8f, 95, 60, 0.5f, 1, 1, 1, 1);
        targets(0, NULL, NULL, 128, 128);
    }
    ID3D11PixelShader *ps = mk_ps(SRC_SH, "psh");
    fb f = mk_fb(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    struct { int fmt; D3D11_FILTER fl; D3D11_COMPARISON_FUNC cmp; float mode; const char *nm; } cf[6] = {
        { 0, D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D11_COMPARISON_LESS, 0, "D32 点 LESS" },
        { 0, D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D11_COMPARISON_LESS, 0, "D32 線形 LESS" },
        { 1, D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D11_COMPARISON_LESS_EQUAL, 0, "D24 線形 LESS_EQUAL" },
        { 2, D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D11_COMPARISON_GREATER, 0, "D16 線形 GREATER" },
        { 0, D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT, D3D11_COMPARISON_LESS, 2, "D32 3x3 PCF（offset つき）" },
        { 1, D3D11_FILTER_COMPARISON_MIN_MAG_MIP_POINT, D3D11_COMPARISON_LESS, 1, "D24 を深度として読む" },
    };
    ID3D11SamplerState *sp = mk_samp(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, NULL, 0);
    for (int k = 0; k < 6; k++) {
        ID3D11SamplerState *sc = mk_samp(cf[k].fl, D3D11_TEXTURE_ADDRESS_CLAMP, NULL, cf[k].cmp);
        fb_begin(&f, 0, 0, 0, 1);
        q_use(ps);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv[cf[k].fmt]);
        ID3D11SamplerState *ss[2] = { sc, sp };
        ID3D11DeviceContext_PSSetSamplers(ctx, 0, 2, ss);
        quad(0, 0, 128, 128, 1, 1, 1, 1, 0.5f, 0.5f, cf[k].mode);
        ID3D11ShaderResourceView *nn = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &nn);
        img m = show(f.ct, 0, 0, 0, 0, cf[k].nm);
        int pts[5][2] = { { 40, 40 }, { 80, 90 }, { 95, 20 }, { 16, 40 }, { 64, 64 } };
        for (int i = 0; i < 5; i++) val_px(&m, pts[i][0], pts[i][1], cf[k].nm);
    }
}

// ---- 10. sRGB ----
static const char *SRC_SR = Q_HEAD
    "Texture2D tx : register(t0); SamplerState sp : register(s0);\n"
    "float4 pst(VO i) : SV_Target { return tx.Sample(sp, float2(i.uv.x, 0.5)); }\n";
static void scene_srgb(void) {
    // 1. sRGB の的に 0〜1 を描く
    fb f = mk_fb(256, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 0, 1);
    fb_begin(&f, 0, 0, 0, 1);
    quad(0, 0, 256, 32, 1, 1, 0.5f, 1, 0.5f, 0.5f, 4);
    img m = show(f.ct, 0, 0, 0, 0, "sRGB の的に 0〜1");
    for (int x = 0; x < 256; x += 32) {
        const uint8_t *p = m.px + ((size_t) 16 * m.w + x) * 4;
        val("sRGB の的 x=%d：%u %u %u %u", x, p[0], p[1], p[2], p[3]);
    }
    // 2. 0.5 で消す → 188 になるはず
    clear_rtv(f.rtv, 0.5f, 0.25f, 0.75f, 0.5f);
    img c = read_tex(f.ct, 0, 0, 0);
    val("sRGB の的を 0.5 0.25 0.75 0.5 で消した：%u %u %u %u", c.px[0], c.px[1], c.px[2], c.px[3]);
    // 3. sRGB の的でのブレンド：黒に白を半分 → 線形で 0.5
    fb_begin(&f, 0, 0, 0, 1);
    set_bs(mk_bs1(TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, 0, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, 0, 15), NULL);
    quad(0, 0, 128, 32, 1, 1, 1, 0.5f, 0.5f, 0.5f, 0);
    set_bs(mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, 15), NULL);
    quad(128, 0, 256, 32, 0.25f, 0.25f, 0.25f, 0.25f, 0.5f, 0.5f, 0);
    quad(128, 0, 256, 32, 0.25f, 0.25f, 0.25f, 0.25f, 0.5f, 0.5f, 0);
    set_bs(bs_off(), NULL);
    img bl = read_tex(f.ct, 0, 0, 0);
    val("sRGB の的でブレンド 白を半分：%u %u %u %u", bl.px[(16 * 256 + 64) * 4], bl.px[(16 * 256 + 64) * 4 + 1], bl.px[(16 * 256 + 64) * 4 + 2], bl.px[(16 * 256 + 64) * 4 + 3]);
    val("sRGB の的でブレンド 0.25 を 2 回足す：%u %u", bl.px[(16 * 256 + 192) * 4], bl.px[(16 * 256 + 192) * 4 + 3]);
    // 4. sRGB のテクスチャを読む（点）→ UNORM の的へ。線形での補間も
    uint8_t ramp[256 * 4], two[2 * 4] = { 0, 0, 0, 255, 255, 255, 255, 255 };
    for (int i = 0; i < 256; i++) rgba(ramp + i * 4, i, 255 - i, i / 2, 255);
    D3D11_SUBRESOURCE_DATA i1 = { ramp, 1024, 0 }, i2 = { two, 8, 0 };
    ID3D11Texture2D *ts = mk_tex(256, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, D3D11_BIND_SHADER_RESOURCE, 0, &i1);
    ID3D11Texture2D *t2 = mk_tex(2, 1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, D3D11_BIND_SHADER_RESOURCE, 0, &i2);
    ID3D11ShaderResourceView *vs = mk_srv(ts, NULL), *v2 = mk_srv(t2, NULL);
    fb u = mk_fb(256, 8, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&u, 0, 0, 0, 1);
    q_use(mk_ps(SRC_SR, "pst"));
    ID3D11SamplerState *sp = mk_samp(D3D11_FILTER_MIN_MAG_MIP_POINT, D3D11_TEXTURE_ADDRESS_CLAMP, NULL, 0);
    ID3D11SamplerState *sl = mk_samp(D3D11_FILTER_MIN_MAG_MIP_LINEAR, D3D11_TEXTURE_ADDRESS_CLAMP, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &vs);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &sp);
    quad(0, 0, 256, 8, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    img r = show(u.ct, 0, 0, 0, 0, "sRGB のテクスチャを読んだ");
    for (int x = 0; x < 256; x += 32) val("sRGB のテクスチャ x=%d：%u %u %u", x + 16, r.px[(4 * 256 + x + 16) * 4], r.px[(4 * 256 + x + 16) * 4 + 1], r.px[(4 * 256 + x + 16) * 4 + 2]);
    // 0 と 255 の 2 画素を真ん中で線形に読む：デコードしてから混ぜれば 0.5 → 128
    fb_begin(&u, 0, 0, 0, 1);
    q_use(mk_ps(SRC_SR, "pst"));
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &v2);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &sl);
    quad(0, 0, 256, 8, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    img r2 = read_tex(u.ct, 0, 0, 0);
    val("sRGB の 0 と 255 の間を線形で（x=128）：%u", r2.px[(4 * 256 + 128) * 4]);
    // 5. TYPELESS：sRGB の view で描き、UNORM の view で読む
    ID3D11Texture2D *tl = mk_tex(64, 8, 1, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS, 1, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, NULL);
    D3D11_RENDER_TARGET_VIEW_DESC rd = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, .ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D };
    ID3D11RenderTargetView *tr = mk_rtv(tl, &rd);
    clear_rtv(tr, 0, 0, 0, 1);
    targets(1, &tr, NULL, 64, 8);
    q_use(NULL);
    quad(0, 0, 64, 8, 0.5f, 0.2f, 0.9f, 1, 0.5f, 0.5f, 0);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { .Format = DXGI_FORMAT_R8G8B8A8_UNORM, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { 0, 1 } };
    ID3D11ShaderResourceView *tu = mk_srv(tl, &sd);
    fb_begin(&u, 0, 0, 0, 1);
    q_use(mk_ps(SRC_SR, "pst"));
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &tu);
    ID3D11DeviceContext_PSSetSamplers(ctx, 0, 1, &sp);
    quad(0, 0, 256, 8, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    img r3 = read_tex(u.ct, 0, 0, 0);
    val("sRGB の view で 0.5 0.2 0.9 を描き UNORM の view で読む：%u %u %u", r3.px[(4 * 256 + 100) * 4], r3.px[(4 * 256 + 100) * 4 + 1], r3.px[(4 * 256 + 100) * 4 + 2]);
    // 6. BGRA8 の sRGB の的
    fb b = mk_fb(256, 16, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, 0, 1);
    fb_begin(&b, 0, 0, 0, 1);
    quad(0, 0, 256, 16, 1, 1, 0.25f, 1, 0.5f, 0.5f, 4);
    img bb = show(b.ct, 0, 0, 0, 0, "BGRA8 sRGB");
    for (int x = 0; x < 256; x += 64) val("BGRA8 sRGB x=%d（B G R A の順）：%u %u %u %u", x, bb.px[(8 * 256 + x) * 4], bb.px[(8 * 256 + x) * 4 + 1], bb.px[(8 * 256 + x) * 4 + 2], bb.px[(8 * 256 + x) * 4 + 3]);
}

// ---- 10. いろいろな形式の的 ----
static void scene_fmt(void) {
    static const DXGI_FORMAT fm[8] = { DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R11G11B10_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                       DXGI_FORMAT_R16G16B16A16_UNORM, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM };
    static const char *nm[8] = { "R10G10B10A2", "R16G16B16A16_FLOAT", "R11G11B10_FLOAT", "R32G32B32A32_FLOAT", "R16G16B16A16_UNORM", "R8_UNORM", "R16G16_FLOAT", "B8G8R8A8_UNORM" };
    for (int k = 0; k < 8; k++) {
        UINT sup = 0;
        ID3D11Device_CheckFormatSupport(dev, fm[k], &sup);
        if (!(sup & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) { val("%s：的にできない", nm[k]); continue; }
        fb f = mk_fb(128, 32, fm[k], 0, 1);
        fb_begin(&f, 0.1f, 0.2f, 0.3f, 0.4f);
        quad(0, 0, 64, 32, 1, 1, 3.0f, 0.5f, 0.5f, 0.5f, 4);
        set_bs(mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, 15), NULL);
        quad(64, 0, 128, 32, 0.5f, 1.5f, 0.25f, 0.5f, 0.5f, 0.5f, 0);
        quad(96, 0, 128, 32, 0.125f, 0.0625f, 2.0f, 0.25f, 0.5f, 0.5f, 0);
        set_bs(bs_off(), NULL);
        img m = show(f.ct, 0, 0, 0, 0, nm[k]);
        int xs[6] = { 0, 21, 42, 63, 80, 110 };
        for (int i = 0; i < 6; i++) val_px(&m, xs[i], 16, nm[k]);
    }
}

// ---- 11. 写し：CopyResource・CopySubresourceRegion・形式の読み替え・バッファ・UpdateSubresource・ステージング・3D ----
static void scene_copy(void) {
    enum { S = 64 };
    uint8_t *pa = malloc(S * S * 4);
    for (int y = 0; y < S; y++)
        for (int x = 0; x < S; x++) rgba(pa + (y * S + x) * 4, x * 4, y * 4, ((x / 8 + y / 8) & 1) * 200 + 30, 255 - x);
    D3D11_SUBRESOURCE_DATA ia = { pa, S * 4, 0 };
    UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *A = mk_tex(S, S, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, bind, 0, &ia);
    // 1. CopyResource
    ID3D11Texture2D *B = mk_tex(S, S, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, bind, 0, NULL);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *) B, (ID3D11Resource *) A);
    img mb = show(B, 0, 0, 0, 1, "CopyResource");
    val("CopyResource が元と同じ：%s", memcmp(mb.px, pa, S * S * 4) ? "違う" : "同じ");
    // 2. CopySubresourceRegion（箱と行き先）
    ID3D11Texture2D *C = mk_tex(S, S, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, bind, 0, NULL);
    ID3D11RenderTargetView *cv = mk_rtv(C, NULL);
    clear_rtv(cv, 0, 0, 1, 1);
    D3D11_BOX box = { 16, 16, 0, 48, 40, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *) C, 0, 8, 40, 0, (ID3D11Resource *) A, 0, &box);
    img mc = show(C, 0, 0, 0, 0, "CopySubresourceRegion");
    val_px(&mc, 8, 40, "箱の写し");
    val_px(&mc, 39, 63, "箱の写し");
    val_px(&mc, 40, 50, "箱の外");
    // 3. mip と層のある物の (mip 1・層 1) へ
    ID3D11Texture2D *D = mk_tex(128, 128, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, 1, bind, 0, NULL);
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *) D, D3D11CalcSubresource(1, 1, 2), 0, 0, 0, (ID3D11Resource *) A, 0, NULL);
    img md = show(D, 1, 1, 0, 0, "mip 1・層 1 への写し");
    val("mip 1・層 1 が元と同じ：%s", memcmp(md.px, pa, S * S * 4) ? "違う" : "同じ");
    // 4. 形式の読み替え：UNORM → UINT（同じ TYPELESS の仲間）
    ID3D11Texture2D *E = mk_tex(S, S, 1, 1, DXGI_FORMAT_R8G8B8A8_UINT, 1, D3D11_BIND_SHADER_RESOURCE, 0, NULL);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *) E, (ID3D11Resource *) A);
    img me = read_tex(E, 0, 0, 0);
    val("UNORM → UINT の写しが元と同じ：%s", memcmp(me.px, pa, S * S * 4) ? "違う" : "同じ");
    // 5. R32_FLOAT → R32_UINT
    float fl[16 * 16];
    for (int i = 0; i < 256; i++) fl[i] = i * 0.5f - 7.25f;
    D3D11_SUBRESOURCE_DATA ifl = { fl, 64, 0 };
    ID3D11Texture2D *F = mk_tex(16, 16, 1, 1, DXGI_FORMAT_R32_FLOAT, 1, D3D11_BIND_SHADER_RESOURCE, 0, &ifl);
    ID3D11Texture2D *G = mk_tex(16, 16, 1, 1, DXGI_FORMAT_R32_UINT, 1, D3D11_BIND_SHADER_RESOURCE, 0, NULL);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *) G, (ID3D11Resource *) F);
    img mg = read_tex(G, 0, 0, 0);
    val("R32_FLOAT → R32_UINT の写しが元と同じ：%s・[5] = %08x", memcmp(mg.px, fl, sizeof fl) ? "違う" : "同じ", img_u32(&mg, 5, 0));
    // 6. バッファの一部を半端な位置へ
    uint8_t b1[1024];
    for (int i = 0; i < 1024; i++) b1[i] = (uint8_t) (i * 7 + 3);
    ID3D11Buffer *buf1 = mk_buf(1024, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0, b1);
    ID3D11Buffer *buf2 = mk_buf(1024, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0, 0, NULL);
    D3D11_BOX bb = { 100, 0, 0, 300, 1, 1 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *) buf2, 0, 37, 0, 0, (ID3D11Resource *) buf1, 0, &bb);
    uint8_t *r2 = read_buf(buf2, 1024);
    int bad = 0;
    for (int i = 0; i < 1024; i++) { uint8_t want = (i >= 37 && i < 237) ? b1[i - 37 + 100] : 0; if (r2[i] != want) bad++; }
    val("バッファの 100〜300 を 37 へ：違うバイト %d", bad);
    // 7. UpdateSubresource：箱つき（行の幅が半端）と mip 1
    ID3D11Texture2D *H = mk_tex(S, S, 2, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, bind, 0, NULL);
    ID3D11RenderTargetView *hv = mk_rtv(H, NULL);
    clear_rtv(hv, 0.5f, 0.5f, 0.5f, 1);
    uint8_t up[100 * 30];
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 100; x++) up[y * 100 + x] = (uint8_t) (x * 3 + y * 5);
    D3D11_BOX ub = { 10, 20, 0, 30, 50, 1 };
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) H, 0, &ub, up, 100, 0);
    uint8_t m1[32 * 32 * 4];
    for (int i = 0; i < 32 * 32; i++) rgba(m1 + i * 4, i & 255, (i >> 2) & 255, 77, 255);
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) H, 1, NULL, m1, 128, 0);
    img mh = show(H, 0, 0, 0, 0, "UpdateSubresource の箱");
    img mh1 = read_tex(H, 1, 0, 0);
    val("UpdateSubresource の mip 1 が同じ：%s", memcmp(mh1.px, m1, sizeof m1) ? "違う" : "同じ");
    bad = 0;
    for (int y = 20; y < 50; y++)
        for (int x = 10; x < 30; x++)
            if (memcmp(mh.px + (y * S + x) * 4, up + (y - 20) * 100 + (x - 10) * 4, 4)) bad++;
    val("UpdateSubresource の箱：違う画素 %d・箱の外 (5,5) = %u", bad, mh.px[(5 * S + 5) * 4]);
    // 8. ステージングに書いて、DEFAULT を通して、別のステージングで読む
    D3D11_TEXTURE2D_DESC sd = { 32, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, { 1, 0 }, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_WRITE | D3D11_CPU_ACCESS_READ, 0 };
    ID3D11Texture2D *S1 = NULL, *S2 = NULL;
    CK(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &S1));
    CK(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &S2));
    D3D11_MAPPED_SUBRESOURCE mp;
    CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) S1, 0, D3D11_MAP_WRITE, 0, &mp));
    for (int y = 0; y < 32; y++)
        for (int x = 0; x < 32; x++) rgba((uint8_t *) mp.pData + y * mp.RowPitch + x * 4, x * 8, y * 8, 99, 200);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) S1, 0);
    ID3D11Texture2D *T = mk_tex(32, 32, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_SHADER_RESOURCE, 0, NULL);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *) T, (ID3D11Resource *) S1);
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *) S2, (ID3D11Resource *) T);
    // 9. DO_NOT_WAIT で読めるまで回す
    int tries = 0;
    HRESULT hr;
    for (;;) {
        hr = ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) S2, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mp);
        if (hr != DXGI_ERROR_WAS_STILL_DRAWING || ++tries > 1000000) break;
    }
    if (SUCCEEDED(hr)) {
        bad = 0;
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                const uint8_t *p = (const uint8_t *) mp.pData + y * mp.RowPitch + x * 4;
                if (p[0] != (uint8_t) (x * 8) || p[1] != (uint8_t) (y * 8) || p[2] != 99 || p[3] != 200) bad++;
            }
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) S2, 0);
        val("ステージング → DEFAULT → ステージング：違う画素 %d", bad);
    } else val("DO_NOT_WAIT の Map が失敗 0x%08lx", (unsigned long) hr);
    info("DO_NOT_WAIT の Map の回数 %d", tries);
    // 10. 3D テクスチャの箱の写し
    uint8_t v3[8 * 8 * 8];
    for (int i = 0; i < 512; i++) v3[i] = (uint8_t) (i * 13 + 1);
    D3D11_TEXTURE3D_DESC d3 = { 8, 8, 8, 1, DXGI_FORMAT_R8_UNORM, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0 };
    D3D11_SUBRESOURCE_DATA i3 = { v3, 8, 64 };
    ID3D11Texture3D *X = NULL, *Y = NULL, *Z = NULL;
    CK(ID3D11Device_CreateTexture3D(dev, &d3, &i3, &X));
    CK(ID3D11Device_CreateTexture3D(dev, &d3, NULL, &Y));
    D3D11_BOX b3 = { 2, 1, 3, 6, 5, 7 };
    ID3D11DeviceContext_CopySubresourceRegion(ctx, (ID3D11Resource *) Y, 0, 1, 2, 0, (ID3D11Resource *) X, 0, &b3);
    d3.Usage = D3D11_USAGE_STAGING; d3.BindFlags = 0; d3.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CK(ID3D11Device_CreateTexture3D(dev, &d3, NULL, &Z));
    ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *) Z, (ID3D11Resource *) Y);
    CK(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *) Z, 0, D3D11_MAP_READ, 0, &mp));
    bad = 0;
    for (int z = 0; z < 4; z++)
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) {
                uint8_t got = ((const uint8_t *) mp.pData)[(z + 0) * mp.DepthPitch + (y + 2) * mp.RowPitch + (x + 1)];
                if (got != v3[(z + 3) * 64 + (y + 1) * 8 + (x + 2)]) bad++;
            }
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *) Z, 0);
    val("3D の箱の写し：違う値 %d / 64", bad);
}

// ---- 12. GenerateMips ----
static void gm_one(DXGI_FORMAT f, int w, int h, int arr, const char *nm, int panels) {
    int bpp = fmt_bpp(f);
    uint8_t *p = calloc((size_t) w * h * arr, (size_t) bpp);
    for (int s = 0; s < arr; s++)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                uint8_t *q = p + ((size_t) s * w * h + (size_t) y * w + x) * bpp;
                float c[4] = { ((x / 8 + y / 8) & 1) ? 1.0f : 0.1f, (float) x / w, (float) y / h * (s + 1) * 0.5f, 1 };
                if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) for (int i = 0; i < 4; i++) { uint16_t hv = f2h(c[i] * 2); memcpy(q + i * 2, &hv, 2); }
                else rgba(q, to8(c[0]), to8(c[1]), to8(c[2]), 255);
            }
    D3D11_TEXTURE2D_DESC td = { (UINT) w, (UINT) h, 0, (UINT) arr, f, { 1, 0 }, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 0, D3D11_RESOURCE_MISC_GENERATE_MIPS };
    ID3D11Texture2D *t = NULL;
    CK(ID3D11Device_CreateTexture2D(dev, &td, NULL, &t));
    ID3D11Texture2D_GetDesc(t, &td);
    for (int s = 0; s < arr; s++)
        ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *) t, D3D11CalcSubresource(0, (UINT) s, td.MipLevels), NULL, p + (size_t) s * w * h * bpp, (UINT) (w * bpp), 0);
    ID3D11ShaderResourceView *v = mk_srv(t, NULL);
    ID3D11DeviceContext_GenerateMips(ctx, v);
    for (int s = arr - 1; s < arr; s++)
        for (UINT m = 1; m < td.MipLevels; m++) {
            char lb[96];
            snprintf(lb, sizeof lb, "%s 層 %d mip %u", nm, s, m);
            img im = (int) m <= panels ? show(t, m, (UINT) s, 0, 0, lb) : read_tex(t, m, (UINT) s, 0);
            double sum[4] = { 0 };
            for (int y = 0; y < im.h; y++)
                for (int x = 0; x < im.w; x++) {
                    float o[4];
                    img_get(&im, x, y, o);
                    for (int i = 0; i < 4; i++) sum[i] += o[i];
                }
            int n = im.w * im.h;
            val("%s（%dx%d）の平均 %.3f %.3f %.3f %.3f", lb, im.w, im.h, sum[0] / n, sum[1] / n, sum[2] / n, sum[3] / n);
            if (im.w >= 4) val_px(&im, im.w / 2, im.h / 2, lb);
        }
}
static void scene_genmips(void) {
    gm_one(DXGI_FORMAT_R8G8B8A8_UNORM, 128, 128, 1, "RGBA8", 4);
    gm_one(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 128, 128, 1, "RGBA8 sRGB", 3);
    gm_one(DXGI_FORMAT_R16G16B16A16_FLOAT, 64, 64, 1, "RGBA16F", 2);
    gm_one(DXGI_FORMAT_R8G8B8A8_UNORM, 100, 60, 1, "2 の冪でない 100x60", 4);
    gm_one(DXGI_FORMAT_R8G8B8A8_UNORM, 32, 32, 2, "配列 2 層", 2);
}
