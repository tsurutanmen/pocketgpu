// 的まわりの場面：深度・ステンシル・ブレンド・MSAA・MRT・デュアルソース・ラスタライザ

// 色の的と深度の的をまとめて作る
typedef struct { ID3D11Texture2D *ct, *dt; ID3D11RenderTargetView *rtv; ID3D11DepthStencilView *dsv; int w, h; } fb;
static fb mk_fb(int w, int h, DXGI_FORMAT cf, DXGI_FORMAT df, UINT samples) {
    fb f = { .w = w, .h = h };
    f.ct = mk_tex((UINT) w, (UINT) h, 1, 1, cf, samples, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, NULL);
    f.rtv = mk_rtv(f.ct, NULL);
    if (df) {
        f.dt = mk_tex((UINT) w, (UINT) h, 1, 1, df, samples, D3D11_BIND_DEPTH_STENCIL, 0, NULL);
        f.dsv = mk_dsv(f.dt, NULL);
    }
    return f;
}
static void fb_begin(fb *f, float r, float g, float b, float a) {
    clear_rtv(f->rtv, r, g, b, a);
    if (f->dsv) ID3D11DeviceContext_ClearDepthStencilView(ctx, f->dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    targets(1, &f->rtv, f->dsv, f->w, f->h);
    q_use(NULL);
}
static void set_ds(ID3D11DepthStencilState *s, UINT ref) { ID3D11DeviceContext_OMSetDepthStencilState(ctx, s, ref); }
static void set_bs(ID3D11BlendState *b, const float *factor) {
    float z[4] = { 0 };
    ID3D11DeviceContext_OMSetBlendState(ctx, b, factor ? factor : z, 0xffffffff);
}

// ---- 1. 深度：比べ方 8 種類・書き込みなし・深度テストなし。深度の形式 4 種類 ----
static void scene_depth(void) {
    static const DXGI_FORMAT dfs[4] = { DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D32_FLOAT_S8X24_UINT };
    static const char *names[4] = { "D24S8", "D32F", "D16", "D32FS8" };
    for (int k = 0; k < 4; k++) {
        fb f = mk_fb(320, 64, DXGI_FORMAT_R8G8B8A8_UNORM, dfs[k], 1);
        fb_begin(&f, 0.02f, 0.02f, 0.02f, 1);
        for (int c = 0; c < 8; c++) {
            float x0 = (float) c * 32;
            set_ds(mk_dss(TRUE, TRUE, D3D11_COMPARISON_ALWAYS, 0, 0, 0xff, 0xff), 0);
            quad(x0, 0, x0 + 32, 64, 0.4f, 0.4f, 0.4f, 1, 0.5f, 0.5f, 0);
            set_ds(mk_dss(TRUE, TRUE, (D3D11_COMPARISON_FUNC) (D3D11_COMPARISON_NEVER + c), 0, 0, 0xff, 0xff), 0);
            quad(x0, 2, x0 + 32, 20, 1, 0.2f, 0.2f, 1, 0.25f, 0.25f, 0);
            quad(x0, 22, x0 + 32, 40, 0.2f, 1, 0.2f, 1, 0.5f, 0.5f, 0);
            quad(x0, 42, x0 + 32, 58, 0.2f, 0.3f, 1, 1, 0.75f, 0.75f, 0);
            // 下の細い帯：深度が左 0 から右 1 へ変わる。背景の 0.5 と LESS で比べ、書かない
            set_ds(mk_dss(TRUE, FALSE, D3D11_COMPARISON_LESS, 0, 0, 0xff, 0xff), 0);
            quad(x0, 59, x0 + 32, 64, 1, 1, 0, 1, 0, 1, 0);
        }
        // 書き込みなし：0.3 を書かずに描き、そのあと 0.4 が通る
        set_ds(mk_dss(TRUE, TRUE, D3D11_COMPARISON_ALWAYS, 0, 0, 0xff, 0xff), 0);
        quad(256, 0, 288, 64, 0.4f, 0.4f, 0.4f, 1, 0.5f, 0.5f, 0);
        set_ds(mk_dss(TRUE, FALSE, D3D11_COMPARISON_LESS, 0, 0, 0xff, 0xff), 0);
        quad(256, 0, 288, 64, 1, 1, 0, 1, 0.3f, 0.3f, 0);
        set_ds(mk_dss(TRUE, TRUE, D3D11_COMPARISON_LESS, 0, 0, 0xff, 0xff), 0);
        quad(256, 16, 288, 48, 0, 1, 1, 1, 0.4f, 0.4f, 0);
        // 深度テストなし：0.2 で描いても深度は書かれず、そのあと 0.3 が通る
        set_ds(mk_dss(TRUE, TRUE, D3D11_COMPARISON_ALWAYS, 0, 0, 0xff, 0xff), 0);
        quad(288, 0, 320, 64, 0.4f, 0.4f, 0.4f, 1, 0.5f, 0.5f, 0);
        set_ds(mk_dss(FALSE, TRUE, D3D11_COMPARISON_LESS, 0, 0, 0xff, 0xff), 0);
        quad(288, 0, 320, 64, 1, 0, 1, 1, 0.2f, 0.2f, 0);
        set_ds(mk_dss(TRUE, TRUE, D3D11_COMPARISON_LESS, 0, 0, 0xff, 0xff), 0);
        quad(288, 16, 320, 48, 1, 1, 1, 1, 0.3f, 0.3f, 0);
        show(f.ct, 0, 0, 0, 0, names[k]);
        img d = read_tex(f.dt, 0, 0, 0);
        char lb[64];
        snprintf(lb, sizeof lb, "%s の深度", names[k]);
        int pts[6][2] = { { 48, 10 }, { 48, 30 }, { 48, 50 }, { 272, 32 }, { 304, 32 }, { 304, 5 } };
        for (int i = 0; i < 6; i++) {
            float o[4];
            img_get(&d, pts[i][0], pts[i][1], o);
            val("%s (%d,%d) = %.4f", lb, pts[i][0], pts[i][1], o[0]);
        }
    }
}

// ---- 1. ステンシル：切り抜き・数える・書き込みの制限・読みの制限・表と裏で別の操作・シェーダーで読む ----
static const char *SRC_STV = Q_HEAD
    "Texture2D<uint2> st : register(t0);\n"
    "float4 psv(VO i) : SV_Target { uint s = st.Load(int3(i.pos.xy, 0)).g;\n"
    "  return float4((s & 3) / 3.0, ((s >> 2) & 3) / 3.0, ((s >> 4) & 15) / 15.0, 1); }\n";
static void scene_stencil(void) {
    fb f = mk_fb(256, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    ID3D11Texture2D *dt = mk_tex(256, 128, 1, 1, DXGI_FORMAT_R24G8_TYPELESS, 1, D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 0, NULL);
    D3D11_DEPTH_STENCIL_VIEW_DESC dd = { .Format = DXGI_FORMAT_D24_UNORM_S8_UINT, .ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D };
    f.dsv = mk_dsv(dt, &dd);
    f.dt = dt;
    fb_begin(&f, 0.05f, 0.05f, 0.05f, 1);
    ID3D11BlendState *nocol = mk_bs1(FALSE, 0, 0, 0, 0, 0, 0, 0);
#define SDS(fn, op, rm, wm) mk_dss(FALSE, FALSE, 0, fn, op, rm, wm)
    // 左半分：丸の形に 1 を書き、EQUAL で緑・NOT_EQUAL で暗い赤
    set_bs(nocol, NULL);
    set_ds(SDS(D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_REPLACE, 0xff, 0xff), 1);
    quad(0, 0, 128, 128, 1, 1, 1, 1, 0.5f, 0.5f, 3);
    set_bs(bs_off(), NULL);
    set_ds(SDS(D3D11_COMPARISON_EQUAL, D3D11_STENCIL_OP_KEEP, 0xff, 0xff), 1);
    quad(0, 0, 128, 128, 0.2f, 0.9f, 0.3f, 1, 0.5f, 0.5f, 0);
    set_ds(SDS(D3D11_COMPARISON_NOT_EQUAL, D3D11_STENCIL_OP_KEEP, 0xff, 0xff), 1);
    quad(0, 0, 128, 128, 0.4f, 0, 0, 1, 0.5f, 0.5f, 0);
    // 右上の左：INCR_SAT で重なりを数え、1・2・3 を赤・緑・青に
    set_bs(nocol, NULL);
    set_ds(SDS(D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_INCR_SAT, 0xff, 0xff), 0);
    quad(128, 0, 176, 48, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    quad(144, 16, 192, 64, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    quad(136, 8, 184, 56, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    set_bs(bs_off(), NULL);
    for (int k = 1; k <= 3; k++) {
        set_ds(SDS(D3D11_COMPARISON_EQUAL, D3D11_STENCIL_OP_KEEP, 0xff, 0xff), (UINT) k);
        quad(128, 0, 192, 64, k == 1 ? 1.f : 0.f, k == 2 ? 1.f : 0.f, k == 3 ? 1.f : 0.f, 1, 0.5f, 0.5f, 0);
    }
    // 右上の右：書き込みの制限 0x06 で 0xff を書く → 6 になる
    set_bs(nocol, NULL);
    set_ds(SDS(D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_REPLACE, 0xff, 0x06), 0xff);
    quad(196, 4, 252, 60, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    set_bs(bs_off(), NULL);
    set_ds(SDS(D3D11_COMPARISON_EQUAL, D3D11_STENCIL_OP_KEEP, 0xff, 0xff), 6);
    quad(192, 0, 256, 64, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    set_ds(SDS(D3D11_COMPARISON_NOT_EQUAL, D3D11_STENCIL_OP_KEEP, 0xff, 0xff), 6);
    quad(192, 0, 256, 64, 0, 0, 0.4f, 1, 0.5f, 0.5f, 0);
    // 右下の左：1・2・3 の帯。読みの制限 0x01 で ref 1 と EQUAL → 1 と 3 が黄色
    set_bs(nocol, NULL);
    for (int k = 1; k <= 3; k++) {
        set_ds(SDS(D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_REPLACE, 0xff, 0xff), (UINT) k);
        quad(128, 64 + (k - 1) * 21.0f, 192, k == 3 ? 128 : 64 + k * 21.0f, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    }
    set_bs(bs_off(), NULL);
    set_ds(SDS(D3D11_COMPARISON_EQUAL, D3D11_STENCIL_OP_KEEP, 0x01, 0xff), 1);
    quad(128, 64, 192, 128, 1, 1, 0, 1, 0.5f, 0.5f, 0);
    // 右下の右：表と裏で別の操作（表は INCR・裏は DECR）。0x10 から始める
    set_bs(nocol, NULL);
    set_ds(SDS(D3D11_COMPARISON_ALWAYS, D3D11_STENCIL_OP_REPLACE, 0xff, 0xff), 0x10);
    quad(192, 64, 256, 128, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    {
        D3D11_DEPTH_STENCIL_DESC d = { .DepthEnable = FALSE, .StencilEnable = TRUE, .StencilReadMask = 0xff, .StencilWriteMask = 0xff,
                                       .FrontFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_INCR, D3D11_COMPARISON_ALWAYS },
                                       .BackFace = { D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_KEEP, D3D11_STENCIL_OP_DECR, D3D11_COMPARISON_ALWAYS } };
        ID3D11DepthStencilState *s = NULL;
        CK(ID3D11Device_CreateDepthStencilState(dev, &d, &s));
        set_ds(s, 0);
        quad(196, 68, 222, 124, 1, 1, 1, 1, 0.5f, 0.5f, 0); // 表（時計回り）
        quad(252, 68, 226, 124, 1, 1, 1, 1, 0.5f, 0.5f, 0); // 裏（左右を入れ替えた）
    }
    set_bs(bs_off(), NULL);
    set_ds(SDS(D3D11_COMPARISON_EQUAL, D3D11_STENCIL_OP_KEEP, 0xff, 0xff), 0x11);
    quad(192, 64, 256, 128, 0, 1, 0, 1, 0.5f, 0.5f, 0);
    set_ds(SDS(D3D11_COMPARISON_EQUAL, D3D11_STENCIL_OP_KEEP, 0xff, 0xff), 0x0f);
    quad(192, 64, 256, 128, 0, 0, 1, 1, 0.5f, 0.5f, 0);
    set_ds(SDS(D3D11_COMPARISON_EQUAL, D3D11_STENCIL_OP_KEEP, 0xff, 0xff), 0x10);
    quad(192, 64, 256, 128, 0.5f, 0.5f, 0.5f, 1, 0.5f, 0.5f, 0);
#undef SDS
    show(f.ct, 0, 0, 0, 0, "ステンシルで描いた色");
    // ステンシルの中身を読む（ステージング）
    img d = read_tex(dt, 0, 0, DXGI_FORMAT_D24_UNORM_S8_UINT);
    int pts[10][2] = { { 64, 64 }, { 2, 2 }, { 130, 2 }, { 160, 32 }, { 150, 20 }, { 220, 30 }, { 160, 75 }, { 160, 100 }, { 209, 96 }, { 239, 96 } };
    for (int i = 0; i < 10; i++) val("ステンシル (%d,%d) = %u", pts[i][0], pts[i][1], img_u32(&d, pts[i][0], pts[i][1]) >> 24);
    // シェーダーでステンシルを読む（X24_TYPELESS_G8_UINT）
    fb v = mk_fb(256, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&v, 0, 0, 0, 1);
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = { .Format = DXGI_FORMAT_X24_TYPELESS_G8_UINT, .ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D, .Texture2D = { 0, 1 } };
    ID3D11ShaderResourceView *srv = mk_srv(dt, &sd);
    q_use(mk_ps(SRC_STV, "psv"));
    ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &srv);
    quad(0, 0, 256, 128, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    show(v.ct, 0, 0, 0, 0, "シェーダーで読んだステンシル");
}

// ---- 2. ブレンド ----
static const char *SRC_B2 = Q_HEAD
    "struct PO { float4 a : SV_Target0; float4 b : SV_Target1; };\n"
    "PO ps2(VO i) { float4 c = i.col; if (ex.x == 1) c.a *= i.uv.y; if (ex.x == 2) c.rgb *= float3(i.uv.x, i.uv.y, 1 - i.uv.x);\n"
    "  PO o; o.a = c; o.b = float4(c.b, c.r, c.g, c.a); return o; }\n";
static void scene_blend(void) {
    fb f = mk_fb(320, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&f, 0, 0, 0, 0);
    float bf[4] = { 0.25f, 0.5f, 0.75f, 0.5f };
    ID3D11BlendState *bss[10] = {
        mk_bs1(TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, 0, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, 0, 15),
        mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, 15),
        mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, 0, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, 0, 15),
        mk_bs1(TRUE, D3D11_BLEND_DEST_COLOR, D3D11_BLEND_ZERO, 0, D3D11_BLEND_DEST_ALPHA, D3D11_BLEND_ZERO, 0, 15),
        mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_REV_SUBTRACT, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_REV_SUBTRACT, 15),
        mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_SUBTRACT, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_SUBTRACT, 15),
        mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MAX, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_MIN, 15),
        mk_bs1(TRUE, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, 0, D3D11_BLEND_BLEND_FACTOR, D3D11_BLEND_INV_BLEND_FACTOR, 0, 15),
        mk_bs1(FALSE, 0, 0, 0, 0, 0, 0, D3D11_COLOR_WRITE_ENABLE_RED | D3D11_COLOR_WRITE_ENABLE_ALPHA),
        mk_bs1(TRUE, D3D11_BLEND_INV_DEST_ALPHA, D3D11_BLEND_DEST_ALPHA, 0, D3D11_BLEND_SRC_ALPHA_SAT, D3D11_BLEND_ZERO, 0, 15),
    };
    for (int c = 0; c < 10; c++) {
        float x0 = (float) c * 32;
        set_bs(bs_off(), NULL);
        quad(x0, 0, x0 + 32, 64, 1, 1, 1, 0.6f, 0.5f, 0.5f, 2);
        set_bs(bss[c], bf);
        quad(x0, 8, x0 + 32, 56, 0.9f, 0.3f, 0.1f, 1, 0.5f, 0.5f, 1);
    }
    img m = show(f.ct, 0, 0, 0, 1, "ブレンド 10 種類");
    for (int c = 0; c < 10; c++) val_px(&m, c * 32 + 20, 40, "ブレンド");
    // 独立ブレンド：的 2 枚で、0 はアルファ・1 は加算で緑とアルファだけ書く
    fb a = mk_fb(160, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1), b = mk_fb(160, 64, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    clear_rtv(a.rtv, 0, 0, 0, 0);
    clear_rtv(b.rtv, 0.1f, 0.1f, 0.1f, 0.2f);
    ID3D11RenderTargetView *two[2] = { a.rtv, b.rtv };
    targets(2, two, NULL, 160, 64);
    q_use(mk_ps(SRC_B2, "ps2"));
    quad(0, 0, 160, 64, 1, 1, 1, 0.6f, 0.5f, 0.5f, 2);
    D3D11_BLEND_DESC bd = { .IndependentBlendEnable = TRUE };
    bd.RenderTarget[0] = (D3D11_RENDER_TARGET_BLEND_DESC) { TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_OP_ADD, 15 };
    bd.RenderTarget[1] = (D3D11_RENDER_TARGET_BLEND_DESC) { TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_OP_ADD,
                                                            D3D11_COLOR_WRITE_ENABLE_GREEN | D3D11_COLOR_WRITE_ENABLE_ALPHA };
    ID3D11BlendState *ib = NULL;
    CK(ID3D11Device_CreateBlendState(dev, &bd, &ib));
    set_bs(ib, NULL);
    quad(16, 8, 144, 56, 0.9f, 0.3f, 0.1f, 1, 0.5f, 0.5f, 1);
    img ma = show(a.ct, 0, 0, 0, 1, "独立ブレンド 的 0");
    img mb = show(b.ct, 0, 0, 0, 1, "独立ブレンド 的 1");
    val_px(&ma, 80, 40, "独立 0");
    val_px(&mb, 80, 40, "独立 1");
}

// ---- 3. MSAA：resolve・サンプルを読む・alpha to coverage・FP16・サンプルごとの実行・SV_Coverage ----
static const char *SRC_MS = Q_HEAD
    "Texture2DMS<float4> ms : register(t0);\n"
    "float4 psl(VO i) : SV_Target { return ms.Load(int2(i.pos.xy), (int) ex.y); }\n"
    "float4 pss(VO i, uint s : SV_SampleIndex) : SV_Target { return float4(s == 0, s == 1, s >= 2, 1) * i.col; }\n"
    "float4 psc(VO i, out uint cov : SV_Coverage) : SV_Target { cov = 1; return i.col; }\n";
static void ms_tris(void) {
    set_ds(mk_dss(TRUE, TRUE, D3D11_COMPARISON_LESS, 0, 0, 0xff, 0xff), 0);
    tri(10, 20, 0.3f, 120, 40, 0.7f, 30, 115, 0.5f, 0.9f, 0.2f, 0.2f, 1);
    tri(100, 5, 0.6f, 118, 110, 0.2f, 15, 70, 0.4f, 0.2f, 0.9f, 0.3f, 1);
    tri(64, 10, 0.1f, 124, 124, 0.9f, 4, 100, 0.5f, 0.2f, 0.3f, 0.95f, 1);
}
static void scene_msaa(int n) {
    UINT q = 0;
    ID3D11Device_CheckMultisampleQualityLevels(dev, DXGI_FORMAT_R8G8B8A8_UNORM, (UINT) n, &q);
    info("MSAA %dx の品質の段階 %u", n, q);
    if (!q) die("MSAA %dx を持たない", n);
    fb f = mk_fb(128, 128, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT, (UINT) n);
    ID3D11Texture2D *res = mk_tex(128, 128, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, 1, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, NULL);
    ID3D11RenderTargetView *resv = mk_rtv(res, NULL);
    fb_begin(&f, 0.05f, 0.05f, 0.08f, 1);
    ID3D11DeviceContext_RSSetState(ctx, mk_rs(0, 0, FALSE, 0, 0, 0, TRUE, FALSE, TRUE));
    ms_tris();
    ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *) res, 0, (ID3D11Resource *) f.ct, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    img m = show(res, 0, 0, 0, 0, "resolve");
    int pts[6][2] = { { 40, 60 }, { 100, 60 }, { 64, 100 }, { 37, 23 }, { 110, 50 }, { 64, 64 } };
    for (int i = 0; i < 6; i++) val_px(&m, pts[i][0], pts[i][1], "resolve");
    // サンプルを読む（Texture2DMS の Load）：サンプル 0 と最後
    ID3D11ShaderResourceView *msv = mk_srv(f.ct, NULL);
    ID3D11PixelShader *psl = mk_ps(SRC_MS, "psl");
    for (int s = 0; s < n; s += n - 1) {
        clear_rtv(resv, 0, 0, 0, 1);
        targets(1, &resv, NULL, 128, 128);
        q_use(psl);
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &msv);
        quad4(0, 0, 128, 128, 1, 1, 1, 1, 0.5f, 0.5f, 0, (float) s, 0, 0);
        ID3D11ShaderResourceView *none = NULL;
        ID3D11DeviceContext_PSSetShaderResources(ctx, 0, 1, &none);
        char lb[32];
        snprintf(lb, sizeof lb, "サンプル %d", s);
        show(res, 0, 0, 0, 0, lb);
    }
    // alpha to coverage
    fb_begin(&f, 0, 0, 0, 1);
    D3D11_BLEND_DESC bd = { .AlphaToCoverageEnable = TRUE };
    bd.RenderTarget[0].RenderTargetWriteMask = 15;
    ID3D11BlendState *a2c = NULL;
    CK(ID3D11Device_CreateBlendState(dev, &bd, &a2c));
    set_bs(a2c, NULL);
    quad(8, 8, 120, 120, 1, 1, 1, 1, 0.5f, 0.5f, 1);
    set_bs(bs_off(), NULL);
    ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *) res, 0, (ID3D11Resource *) f.ct, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    show(res, 0, 0, 0, 0, "alpha to coverage");
    // サンプルごとの実行（SV_SampleIndex）
    fb_begin(&f, 0, 0, 0, 1);
    q_use(mk_ps(SRC_MS, "pss"));
    quad(8, 8, 120, 120, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *) res, 0, (ID3D11Resource *) f.ct, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    img ms = show(res, 0, 0, 0, 0, "SV_SampleIndex");
    val_px(&ms, 64, 64, "SV_SampleIndex");
    // SV_Coverage を 1 にして出す（サンプル 0 だけ書く）
    fb_begin(&f, 0, 0, 0, 1);
    q_use(mk_ps(SRC_MS, "psc"));
    quad(8, 8, 120, 120, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *) res, 0, (ID3D11Resource *) f.ct, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    img mc = show(res, 0, 0, 0, 0, "SV_Coverage");
    val_px(&mc, 64, 64, "SV_Coverage");
    // FP16 の MSAA の resolve
    fb h = mk_fb(128, 128, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_D32_FLOAT, (UINT) n);
    ID3D11Texture2D *hres = mk_tex(128, 128, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, 1, D3D11_BIND_RENDER_TARGET, 0, NULL);
    fb_begin(&h, 0.5f, 0.25f, 2.0f, 1);
    ms_tris();
    ID3D11DeviceContext_ResolveSubresource(ctx, (ID3D11Resource *) hres, 0, (ID3D11Resource *) h.ct, 0, DXGI_FORMAT_R16G16B16A16_FLOAT);
    img mh = show(hres, 0, 0, 0, 0, "FP16 resolve");
    for (int i = 0; i < 6; i++) val_px(&mh, pts[i][0], pts[i][1], "FP16 resolve");
}

// ---- 4. 的 4 枚（MRT）。形式を変え、途中で空の口を挟む ----
static const char *SRC_MRT = Q_HEAD
    "struct PO { float4 a : SV_Target0; float4 b : SV_Target1; float4 c : SV_Target2; float d : SV_Target3; };\n"
    "PO psm(VO i) { PO o; float k = ex.x == 5 ? 0.5 : 1;\n"
    "  o.a = float4(i.uv * k, 0.5, 1); o.b = float4(i.uv.x * 4 * k, i.uv.y * 2, -1, 0.25); o.c = float4(1 - i.uv.x, i.uv.y * i.uv.x, 0.75 * k, 0.66);\n"
    "  o.d = i.uv.x * i.uv.y * 10 * k; return o; }\n";
static void scene_mrt(void) {
    static const DXGI_FORMAT fm[4] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R32_FLOAT };
    static const char *nm[4] = { "RGBA8", "RGBA16F", "RGB10A2", "R32F" };
    ID3D11Texture2D *t[4];
    ID3D11RenderTargetView *v[4];
    for (int i = 0; i < 4; i++) {
        t[i] = mk_tex(128, 128, 1, 1, fm[i], 1, D3D11_BIND_RENDER_TARGET, 0, NULL);
        v[i] = mk_rtv(t[i], NULL);
        clear_rtv(v[i], 0.1f * (i + 1), 0.05f, 0.2f, 0.5f);
    }
    targets(4, v, NULL, 128, 128);
    q_use(mk_ps(SRC_MRT, "psm"));
    quad(8, 8, 120, 120, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    ID3D11RenderTargetView *gap[4] = { v[0], NULL, v[2], NULL };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 4, gap, NULL);
    quad(40, 40, 88, 88, 1, 1, 1, 1, 0.5f, 0.5f, 5);
    ID3D11RenderTargetView *last[4] = { NULL, NULL, NULL, v[3] };
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 4, last, NULL);
    quad(60, 10, 70, 118, 1, 1, 1, 1, 0.5f, 0.5f, 5);
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 0, NULL, NULL);
    int pts[4][2] = { { 64, 64 }, { 20, 20 }, { 100, 100 }, { 65, 30 } };
    for (int i = 0; i < 4; i++) {
        img m = show(t[i], 0, 0, 0, 0, nm[i]);
        for (int k = 0; k < 4; k++) val_px(&m, pts[k][0], pts[k][1], nm[i]);
    }
}

// ---- 16. デュアルソースブレンド ----
static const char *SRC_DS = Q_HEAD
    "struct PO { float4 a : SV_Target0; float4 b : SV_Target1; };\n"
    "PO psd(VO i) { PO o; o.a = float4(0.8 * i.uv.x, 0.2, 0.6 * i.uv.y, 0.5); o.b = float4(i.uv.y, 1 - i.uv.x, 0.5, i.uv.x); return o; }\n";
static void scene_dualsrc(void) {
    fb f = mk_fb(256, 128, DXGI_FORMAT_R8G8B8A8_UNORM, 0, 1);
    fb_begin(&f, 0, 0, 0, 1);
    quad(0, 0, 256, 128, 1, 1, 1, 0.8f, 0.5f, 0.5f, 2);
    ID3D11PixelShader *ps = mk_ps(SRC_DS, "psd");
    q_use(ps);
    set_bs(mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_SRC1_COLOR, 0, D3D11_BLEND_ONE, D3D11_BLEND_SRC1_ALPHA, 0, 15), NULL);
    quad(8, 8, 124, 120, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    set_bs(mk_bs1(TRUE, D3D11_BLEND_SRC1_ALPHA, D3D11_BLEND_INV_SRC1_COLOR, 0, D3D11_BLEND_INV_SRC1_ALPHA, D3D11_BLEND_ONE, 0, 15), NULL);
    quad(132, 8, 248, 120, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    img m = show(f.ct, 0, 0, 0, 1, "デュアルソース");
    val_px(&m, 64, 64, "デュアルソース");
    val_px(&m, 190, 64, "デュアルソース");
    val_px(&m, 20, 100, "デュアルソース");
    val_px(&m, 240, 20, "デュアルソース");
}

// ---- 13. ラスタライザ：裏面・ワイヤーフレーム・scissor・深度バイアス・深度クランプ・viewport の深度・線と点 ----
static void scene_raster(void) {
    fb f = mk_fb(256, 256, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_D24_UNORM_S8_UINT, 1);
    fb_begin(&f, 0.03f, 0.03f, 0.03f, 1);
    ID3D11DepthStencilState *less = mk_dss(TRUE, TRUE, D3D11_COMPARISON_LESS, 0, 0, 0xff, 0xff);
    ID3D11DepthStencilState *le = mk_dss(TRUE, TRUE, D3D11_COMPARISON_LESS_EQUAL, 0, 0, 0xff, 0xff);
    ID3D11DepthStencilState *none = mk_dss(FALSE, FALSE, 0, 0, 0, 0xff, 0xff);
#define CX(c) ((float) ((c) % 4) * 64)
#define CY(c) ((float) ((c) / 4) * 64)
    // 0：裏を捨てる・1：表を捨てる・2：反時計回りを表にして裏を捨てる
    for (int c = 0; c < 3; c++) {
        D3D11_CULL_MODE cm = c == 1 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
        ID3D11DeviceContext_RSSetState(ctx, mk_rs(0, cm, c == 2, 0, 0, 0, TRUE, FALSE, FALSE));
        set_ds(none, 0);
        quad(CX(c) + 4, CY(c) + 4, CX(c) + 30, CY(c) + 60, 1, 0.2f, 0.2f, 1, 0.5f, 0.5f, 0);
        quad(CX(c) + 60, CY(c) + 4, CX(c) + 34, CY(c) + 60, 0.2f, 1, 0.2f, 1, 0.5f, 0.5f, 0);
    }
    // 3：ワイヤーフレーム
    ID3D11DeviceContext_RSSetState(ctx, mk_rs(D3D11_FILL_WIREFRAME, D3D11_CULL_NONE, FALSE, 0, 0, 0, TRUE, FALSE, FALSE));
    tri(CX(3) + 8, CY(3) + 8, 0.5f, CX(3) + 56, CY(3) + 20, 0.5f, CX(3) + 20, CY(3) + 56, 0.5f, 1, 1, 1, 1);
    quad(CX(3) + 30, CY(3) + 30, CX(3) + 60, CY(3) + 60, 1, 1, 0, 1, 0.5f, 0.5f, 0);
    // 4：scissor
    ID3D11DeviceContext_RSSetState(ctx, mk_rs(0, 0, FALSE, 0, 0, 0, TRUE, TRUE, FALSE));
    D3D11_RECT sr = { (LONG) CX(4) + 16, (LONG) CY(4) + 16, (LONG) CX(4) + 48, (LONG) CY(4) + 40 };
    ID3D11DeviceContext_RSSetScissorRects(ctx, 1, &sr);
    quad(CX(4), CY(4), CX(4) + 64, CY(4) + 64, 0, 1, 1, 1, 0.5f, 0.5f, 0);
    // 5：深度バイアス（定数）。左は手前へ寄せて通る・右はそのままで通らない
    ID3D11DeviceContext_RSSetState(ctx, rs_def());
    set_ds(less, 0);
    quad(CX(5), CY(5), CX(5) + 64, CY(5) + 64, 0.2f, 0.8f, 0.2f, 1, 0.5f, 0.5f, 0);
    ID3D11DeviceContext_RSSetState(ctx, mk_rs(0, 0, FALSE, -100, 0, 0, TRUE, FALSE, FALSE));
    quad(CX(5), CY(5), CX(5) + 32, CY(5) + 64, 1, 0.2f, 0.2f, 1, 0.5f, 0.5f, 0);
    ID3D11DeviceContext_RSSetState(ctx, rs_def());
    quad(CX(5) + 32, CY(5), CX(5) + 64, CY(5) + 64, 0.2f, 0.2f, 1, 1, 0.5f, 0.5f, 0);
    // 6：傾きの深度バイアス（左は手前・右は奥）
    set_ds(less, 0);
    quad(CX(6), CY(6), CX(6) + 64, CY(6) + 64, 0.2f, 0.8f, 0.2f, 1, 0.2f, 0.8f, 0);
    ID3D11DeviceContext_RSSetState(ctx, mk_rs(0, 0, FALSE, 0, 0, -2.0f, TRUE, FALSE, FALSE));
    quad(CX(6), CY(6), CX(6) + 64, CY(6) + 32, 1, 0.2f, 0.2f, 1, 0.2f, 0.8f, 0);
    ID3D11DeviceContext_RSSetState(ctx, mk_rs(0, 0, FALSE, 0, 0, 2.0f, TRUE, FALSE, FALSE));
    quad(CX(6), CY(6) + 32, CX(6) + 64, CY(6) + 64, 0.2f, 0.2f, 1, 1, 0.2f, 0.8f, 0);
    // 7：バイアスの上限（clamp）
    ID3D11DeviceContext_RSSetState(ctx, mk_rs(0, 0, FALSE, 0, -0.01f, -50.0f, TRUE, FALSE, FALSE));
    set_ds(mk_dss(TRUE, TRUE, D3D11_COMPARISON_ALWAYS, 0, 0, 0xff, 0xff), 0);
    quad(CX(7), CY(7), CX(7) + 64, CY(7) + 64, 0.6f, 0.6f, 0.2f, 1, 0.2f, 0.8f, 0);
    // 8：深度の切り取りをしない（クランプ）・9：する
    for (int c = 8; c <= 9; c++) {
        ID3D11DeviceContext_RSSetState(ctx, mk_rs(0, 0, FALSE, 0, 0, 0, c == 9, FALSE, FALSE));
        set_ds(le, 0);
        quad(CX(c), CY(c), CX(c) + 64, CY(c) + 64, 1, 1, 0, 1, 1.5f, 1.5f, 0);
        quad(CX(c), CY(c), CX(c) + 32, CY(c) + 64, 1, 0, 1, 1, -0.5f, -0.5f, 0);
    }
    // 10：viewport の深度の範囲 0.25〜0.5
    ID3D11DeviceContext_RSSetState(ctx, rs_def());
    set_ds(mk_dss(TRUE, TRUE, D3D11_COMPARISON_ALWAYS, 0, 0, 0xff, 0xff), 0);
    viewport(0, 0, 256, 256, 0.25f, 0.5f);
    quad(CX(10), CY(10), CX(10) + 64, CY(10) + 64, 1, 1, 1, 1, 1.0f, 1.0f, 0);
    viewport(0, 0, 256, 256, 0, 1);
    set_ds(less, 0);
    quad(CX(10) + 32, CY(10), CX(10) + 64, CY(10) + 64, 1, 0, 0, 1, 0.6f, 0.6f, 0);
    quad(CX(10), CY(10), CX(10) + 32, CY(10) + 64, 0, 1, 0, 1, 0.4f, 0.4f, 0);
    // 11：線（LINELIST と LINESTRIP）
    set_ds(none, 0);
    for (int i = 0; i < 8; i++) {
        float a = (float) i * 0.785f, xy[4] = { CX(11) + 32, CY(11) + 32, CX(11) + 32 + 28 * cosf(a), CY(11) + 32 + 28 * sinf(a) };
        prim2(D3D11_PRIMITIVE_TOPOLOGY_LINELIST, 2, xy, 1, 0.5f + 0.06f * i, 0.2f);
    }
    float ls[6] = { CX(11) + 4, CY(11) + 60, CX(11) + 30, CY(11) + 50, CX(11) + 60, CY(11) + 62 };
    prim2(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, 3, ls, 0.3f, 0.7f, 1);
    // 12：点
    for (int i = 0; i < 16; i++) {
        float xy[2] = { CX(12) + 4 + (i % 4) * 16 + i * 0.25f, CY(12) + 4 + (i / 4) * 16 };
        prim2(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, 1, xy, 1, 1, 1);
    }
    // 13：ワイヤーフレームで裏を捨てる
    ID3D11DeviceContext_RSSetState(ctx, mk_rs(D3D11_FILL_WIREFRAME, D3D11_CULL_BACK, FALSE, 0, 0, 0, TRUE, FALSE, FALSE));
    quad(CX(13) + 4, CY(13) + 4, CX(13) + 30, CY(13) + 60, 1, 1, 1, 1, 0.5f, 0.5f, 0);
    quad(CX(13) + 60, CY(13) + 4, CX(13) + 34, CY(13) + 60, 1, 0, 0, 1, 0.5f, 0.5f, 0);
    // 14：辺を分け合う 2 つの三角を加算で描く（左上の決まりで二重に塗らない）
    ID3D11DeviceContext_RSSetState(ctx, rs_def());
    set_bs(mk_bs1(TRUE, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, D3D11_BLEND_ONE, D3D11_BLEND_ONE, 0, 15), NULL);
    tri(CX(14) + 4.3f, CY(14) + 4.7f, 0.5f, CX(14) + 60.2f, CY(14) + 10.1f, 0.5f, CX(14) + 10.6f, CY(14) + 59.4f, 0.5f, 0.5f, 0.3f, 0, 1);
    tri(CX(14) + 60.2f, CY(14) + 10.1f, 0.5f, CX(14) + 58.8f, CY(14) + 61.3f, 0.5f, CX(14) + 10.6f, CY(14) + 59.4f, 0.5f, 0.5f, 0.3f, 0, 1);
    set_bs(bs_off(), NULL);
    img m = show(f.ct, 0, 0, 0, 0, "ラスタライザ 16 区画");
    (void) m;
    img d = read_tex(f.dt, 0, 0, 0);
    int pts[8][2] = { { (int) CX(5) + 16, (int) CY(5) + 32 }, { (int) CX(5) + 48, (int) CY(5) + 32 }, { (int) CX(6) + 32, (int) CY(6) + 16 },
                      { (int) CX(6) + 32, (int) CY(6) + 48 }, { (int) CX(7) + 32, (int) CY(7) + 32 }, { (int) CX(8) + 16, (int) CY(8) + 32 },
                      { (int) CX(8) + 48, (int) CY(8) + 32 }, { (int) CX(10) + 48, (int) CY(10) + 32 } };
    for (int i = 0; i < 8; i++) {
        float o[4];
        img_get(&d, pts[i][0], pts[i][1], o);
        val("深度 (%d,%d) = %.6f", pts[i][0], pts[i][1], o[0]);
    }
#undef CX
#undef CY
}
