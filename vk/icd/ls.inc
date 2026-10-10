// ---- render pass の LOAD・STORE を調べる・減らす（icd.c から取り込む） ----
// タイルで描く GPU（iPhone）は、render pass の始めの LOAD（前の中身をタイルに読む）と終わりの STORE（書き戻す）が遅い。
// command buffer の中の並び（clear → render pass → ...）を見て、要らない LOAD・STORE を外した render pass に差し替える。
//   IPHONE_VK_LSLOG=コマ番号   そのコマの command buffer ごとに、render pass の LOAD・STORE の内訳を stderr に出す（調べる用）
//   IPHONE_VK_LS=数            減らす直しを選ぶ（足し算）。既定は 0（何もしない）
//       1  直前に画像全体を同じ値で消した attachment の LOAD を、CLEAR にする
//       2  pass の頭で、その attachment の描く範囲全部を vkCmdClearAttachments で消していれば、LOAD をやめる（DONT_CARE）
//       4  次に使うときは必ず CLEAR か DONT_CARE で始まる attachment の STORE をやめる（この command buffer の中で決まる物だけ）
//       8  （予約）
//   LOAD・STORE の差し替えは、同じ形（compatible）の render pass を作り直して render pass の番号だけ入れ替える
#define LS_ASPECT_COLOR 1u
#define LS_ASPECT_DEPTH 2u
#define LS_ASPECT_STENCIL 4u

struct lsv { uint32_t kind; struct img *im; uint32_t fmt, base_mip, nmip, base_layer, nlayer, aspect; }; // kind=1
struct lsf { uint32_t kind; uint32_t n, w, h, layers; uint64_t rp; uint64_t v[16]; };                     // kind=2
struct lsatt { uint32_t fmt, samples, load, store, sload, sstore, il, fl; };
struct lsvar { uint32_t mask; uint64_t id; struct lsvar *next; };
struct lsr {                                                                                              // kind=3
    uint32_t kind, rp2, na, nsub;
    struct lsatt a[16];
    uint8_t colref[8]; int dsref;   // subpass 0 の色の番号 → attachment の番号（無ければ 255）・深さ/stencil の attachment の番号（無ければ -1）
    uint8_t ro[16];      // その attachment を読み取り専用の形で使う（CLEAR にできない）
    uint8_t role[16];    // 1 色で書く・2 深さ/stencil で書く・4 resolve の行き先・8 input attachment
    uint8_t *pay; size_t pn;   // 作った命令の中身（送る前の物。変えた render pass を作るのに使う）
    uint64_t dev;
    struct lsvar *var;
    int ok;              // 作り替えてよい形（subpass 1 つ・attachment 10 個まで）
};

static int ls_ro_layout(uint32_t l) {
    return l == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL || l == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL || l == 1000117000 /* DEPTH_READ_ONLY_STENCIL_ATTACHMENT */ ||
           l == 1000117001 /* DEPTH_ATTACHMENT_STENCIL_READ_ONLY */ || l == 1000241001 /* DEPTH_READ_ONLY */ || l == 1000241003 /* STENCIL_READ_ONLY */ || l == 1000314001 /* READ_ONLY */;
}
static SRWLOCK g_ls_lock = SRWLOCK_INIT;
static struct { uint64_t k; void *v; } *g_lsm;
static size_t g_lsm_cap, g_lsm_cnt;
static int g_ls_log_frame = -2;   // IPHONE_VK_LSLOG
static int g_ls_opt = -1;         // IPHONE_VK_LS
static int g_ls_nopend;
static int g_ls_poison;           // IPHONE_VK_LSPOISON=1：書き戻しをやめた画像を終わりで壊す（試験用。やめてよい証拠）
static int g_ls_dbg;              // IPHONE_VK_LSDBG=1：差し替えのたびに記録に出す
static char g_ls_ctl[MAX_PATH];
static int g_ls_ctl_on;

static void ls_init(void) {
    if (g_ls_opt >= 0) return;
    const char *e = getenv("IPHONE_VK_LS");
    g_ls_opt = e && *e ? atoi(e) : 0;
    e = getenv("IPHONE_VK_LSLOG");
    g_ls_log_frame = e && *e ? atoi(e) : -1;
    e = getenv("IPHONE_VK_LSDBG");
    g_ls_dbg = e && *e == '1';
    e = getenv("IPHONE_VK_LSPOISON");
    g_ls_poison = e && *e == '1';
    e = getenv("IPHONE_VK_LSCTL");
    if (e && *e) { snprintf(g_ls_ctl, sizeof g_ls_ctl, "%s", e); g_ls_ctl_on = 1; }
}
// IPHONE_VK_LSCTL=フォルダ：実行中に切り替える（比べる用）。フォルダの opt.txt の数が IPHONE_VK_LS の代わりになり、
// cmd.txt に名前を書くと、次の絵を 名前.ppm に書く（IPHONE_VK_JPEG=0 で使う）
static inline int ls_on(void) { if (g_ls_opt < 0) ls_init(); return g_ls_opt > 0 || g_ls_log_frame >= 0 || g_ls_ctl_on; }
static void ls_ctl_poll(void) {
    static unsigned n;
    if (!g_ls_ctl_on || (n++ & 7)) return;
    char p[MAX_PATH + 16];
    snprintf(p, sizeof p, "%s%sopt.txt", g_ls_ctl, "\\");
    FILE *fp = fopen(p, "rb");
    if (!fp) return;
    char b[32] = { 0 };
    size_t k = fread(b, 1, sizeof b - 1, fp);
    fclose(fp);
    if (k) g_ls_opt = atoi(b);
}
static void ls_ctl_dump(uint32_t w, uint32_t h, const uint8_t *px) {
    if (!g_ls_ctl_on) return;
    char p[MAX_PATH + 16];
    snprintf(p, sizeof p, "%s%scmd.txt", g_ls_ctl, "\\");
    FILE *fp = fopen(p, "rb");
    if (!fp) return;
    char label[96] = { 0 };
    size_t k = fread(label, 1, sizeof label - 1, fp);
    fclose(fp);
    while (k && (label[k - 1] == '\n' || label[k - 1] == '\r' || label[k - 1] == ' ')) label[--k] = 0;
    DeleteFileA(p);
    if (!k) return;
    char q[MAX_PATH + 128];
    snprintf(q, sizeof q, "%s%s%s.ppm", g_ls_ctl, "\\", label);
    FILE *o = fopen(q, "wb");
    if (!o) return;
    fprintf(o, "P6\n%u %u\n255\n", w, h);
    for (size_t i = 0; i < (size_t) w * h; i++) { uint8_t rgb[3] = { px[i * 4 + 2], px[i * 4 + 1], px[i * 4] }; fwrite(rgb, 1, 3, o); }
    fclose(o);
    say("[ls] %s に絵を書いた（opt=%d）", q, g_ls_opt);
}

static void *ls_get_locked(uint64_t k) {
    if (!g_lsm_cap) return NULL;
    size_t m = g_lsm_cap - 1, i = (size_t) (k * 0x9E3779B97F4A7C15ull >> 20) & m;
    while (g_lsm[i].k) {
        if (g_lsm[i].k == k) return g_lsm[i].v;
        i = (i + 1) & m;
    }
    return NULL;
}
static void *ls_get(uint64_t k) {
    AcquireSRWLockShared(&g_ls_lock);
    void *v = ls_get_locked(k);
    ReleaseSRWLockShared(&g_ls_lock);
    return v;
}
static void ls_put(uint64_t k, void *v) {
    AcquireSRWLockExclusive(&g_ls_lock);
    if ((g_lsm_cnt + 1) * 2 > g_lsm_cap) {
        size_t nc = g_lsm_cap ? g_lsm_cap * 2 : 4096;
        typeof(g_lsm) n = calloc(nc, sizeof *n);
        for (size_t i = 0; i < g_lsm_cap; i++)
            if (g_lsm[i].k && g_lsm[i].v) {
                size_t j = (size_t) (g_lsm[i].k * 0x9E3779B97F4A7C15ull >> 20) & (nc - 1);
                while (n[j].k) j = (j + 1) & (nc - 1);
                n[j] = g_lsm[i];
            }
        free(g_lsm);
        g_lsm = n;
        g_lsm_cap = nc;
        g_lsm_cnt = 0;
        for (size_t i = 0; i < nc; i++) g_lsm_cnt += g_lsm[i].k != 0;
    }
    size_t m = g_lsm_cap - 1, i = (size_t) (k * 0x9E3779B97F4A7C15ull >> 20) & m;
    while (g_lsm[i].k && g_lsm[i].k != k) i = (i + 1) & m;
    if (!g_lsm[i].k) g_lsm_cnt++;
    g_lsm[i].k = k;
    g_lsm[i].v = v;
    ReleaseSRWLockExclusive(&g_ls_lock);
}
static void ls_drop(uint64_t id) {
    if (!ls_on()) return;
    AcquireSRWLockExclusive(&g_ls_lock);
    if (g_lsm_cap) {
        size_t m = g_lsm_cap - 1, i = (size_t) (id * 0x9E3779B97F4A7C15ull >> 20) & m;
        while (g_lsm[i].k) {
            if (g_lsm[i].k == id) {
                void *v = g_lsm[i].v;
                g_lsm[i].v = NULL; // 番号は二度と使わないので、札は残して中身だけ消す
                if (v && ((struct lsr *) v)->kind == 3) {
                    struct lsr *r = v;
                    free(r->pay);
                    for (struct lsvar *x = r->var, *nx; x; x = nx) { nx = x->next; free(x); }
                }
                free(v);
                break;
            }
            i = (i + 1) & m;
        }
    }
    ReleaseSRWLockExclusive(&g_ls_lock);
}

static uint32_t ls_aspect_of(VkFormat f) {
    if (f >= VK_FORMAT_D16_UNORM && f <= VK_FORMAT_D32_SFLOAT_S8_UINT) {
        if (f == VK_FORMAT_S8_UINT) return LS_ASPECT_STENCIL;
        if (f == VK_FORMAT_D16_UNORM_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT) return LS_ASPECT_DEPTH | LS_ASPECT_STENCIL;
        return LS_ASPECT_DEPTH;
    }
    return LS_ASPECT_COLOR;
}

static void ls_note_view(uint64_t id, const VkImageViewCreateInfo *ci) {
    if (!ls_on()) return;
    struct lsv *v = calloc(1, sizeof *v);
    v->kind = 1;
    v->im = (struct img *) ci->image;
    v->fmt = ci->format;
    v->base_mip = ci->subresourceRange.baseMipLevel;
    v->nmip = ci->subresourceRange.levelCount;
    v->base_layer = ci->subresourceRange.baseArrayLayer;
    v->nlayer = ci->subresourceRange.layerCount;
    v->aspect = ci->subresourceRange.aspectMask;
    ls_put(id, v);
}

static void ls_note_fb(uint64_t id, const VkFramebufferCreateInfo *ci) {
    if (!ls_on()) return;
    struct lsf *f = calloc(1, sizeof *f);
    f->kind = 2;
    f->n = ci->attachmentCount < 16 ? ci->attachmentCount : 16;
    for (uint32_t i = 0; i < f->n; i++) f->v[i] = ID(ci->pAttachments[i]);
    f->w = ci->width; f->h = ci->height; f->layers = ci->layers;
    f->rp = ID(ci->renderPass);
    ls_put(id, f);
}

static void ls_rp_common(struct lsr *r, const vr_buf *pay, uint64_t id) {
    r->pay = malloc(pay->n);
    memcpy(r->pay, pay->p, pay->n);
    r->pn = pay->n;
    (void) id;
}

static void ls_note_rp1(uint64_t id, const VkRenderPassCreateInfo *ci, const vr_buf *pay) {
    if (!ls_on()) return;
    struct lsr *r = calloc(1, sizeof *r);
    r->kind = 3;
    r->na = ci->attachmentCount < 16 ? ci->attachmentCount : 16;
    r->nsub = ci->subpassCount;
    for (uint32_t i = 0; i < r->na; i++) {
        const VkAttachmentDescription *d = &ci->pAttachments[i];
        r->a[i] = (struct lsatt) { d->format, d->samples, lso_load((uint32_t) d->loadOp), lso_store((uint32_t) d->storeOp), lso_load((uint32_t) d->stencilLoadOp), lso_store((uint32_t) d->stencilStoreOp), d->initialLayout, d->finalLayout };
    }
    for (uint32_t s = 0; s < ci->subpassCount; s++) {
        const VkSubpassDescription *sp = &ci->pSubpasses[s];
        if (s == 0) {
            memset(r->colref, 255, sizeof r->colref);
            r->dsref = sp->pDepthStencilAttachment ? (int) sp->pDepthStencilAttachment->attachment : -1;
            for (uint32_t k = 0; k < sp->colorAttachmentCount && k < 8; k++) r->colref[k] = (uint8_t) (sp->pColorAttachments[k].attachment < 255 ? sp->pColorAttachments[k].attachment : 255);
        }
        for (uint32_t k = 0; k < sp->colorAttachmentCount; k++) if (sp->pColorAttachments[k].attachment < 16) r->role[sp->pColorAttachments[k].attachment] |= 1;
        if (sp->pResolveAttachments) for (uint32_t k = 0; k < sp->colorAttachmentCount; k++) if (sp->pResolveAttachments[k].attachment < 16) r->role[sp->pResolveAttachments[k].attachment] |= 4;
        if (sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment < 16) r->role[sp->pDepthStencilAttachment->attachment] |= 2;
        for (uint32_t k = 0; k < sp->colorAttachmentCount; k++) if (sp->pColorAttachments[k].attachment < 16 && ls_ro_layout(sp->pColorAttachments[k].layout)) r->ro[sp->pColorAttachments[k].attachment] = 1;
        if (sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment < 16 && ls_ro_layout(sp->pDepthStencilAttachment->layout)) r->ro[sp->pDepthStencilAttachment->attachment] = 1;
        for (uint32_t k = 0; k < sp->inputAttachmentCount; k++) if (sp->pInputAttachments[k].attachment < 16) r->role[sp->pInputAttachments[k].attachment] |= 8;
    }
    r->ok = ci->subpassCount == 1 && ci->attachmentCount <= 10;
    r->rp2 = 0;
    ls_rp_common(r, pay, id);
    ls_put(id, r);
}

static void ls_note_rp2(uint64_t id, const VkRenderPassCreateInfo2 *ci, const vr_buf *pay) {
    if (!ls_on()) return;
    struct lsr *r = calloc(1, sizeof *r);
    r->kind = 3;
    r->na = ci->attachmentCount < 16 ? ci->attachmentCount : 16;
    r->nsub = ci->subpassCount;
    for (uint32_t i = 0; i < r->na; i++) {
        const VkAttachmentDescription2 *d = &ci->pAttachments[i];
        r->a[i] = (struct lsatt) { d->format, d->samples, lso_load((uint32_t) d->loadOp), lso_store((uint32_t) d->storeOp), lso_load((uint32_t) d->stencilLoadOp), lso_store((uint32_t) d->stencilStoreOp), d->initialLayout, d->finalLayout };
    }
    int ext = 0;
    for (uint32_t s = 0; s < ci->subpassCount; s++) {
        const VkSubpassDescription2 *sp = &ci->pSubpasses[s];
        if (s == 0) {
            memset(r->colref, 255, sizeof r->colref);
            r->dsref = sp->pDepthStencilAttachment ? (int) sp->pDepthStencilAttachment->attachment : -1;
            for (uint32_t k = 0; k < sp->colorAttachmentCount && k < 8; k++) r->colref[k] = (uint8_t) (sp->pColorAttachments[k].attachment < 255 ? sp->pColorAttachments[k].attachment : 255);
        }
        for (uint32_t k = 0; k < sp->colorAttachmentCount; k++) if (sp->pColorAttachments[k].attachment < 16) r->role[sp->pColorAttachments[k].attachment] |= 1;
        if (sp->pResolveAttachments) for (uint32_t k = 0; k < sp->colorAttachmentCount; k++) if (sp->pResolveAttachments[k].attachment < 16) r->role[sp->pResolveAttachments[k].attachment] |= 4;
        if (sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment < 16) r->role[sp->pDepthStencilAttachment->attachment] |= 2;
        for (uint32_t k = 0; k < sp->colorAttachmentCount; k++) if (sp->pColorAttachments[k].attachment < 16 && ls_ro_layout(sp->pColorAttachments[k].layout)) r->ro[sp->pColorAttachments[k].attachment] = 1;
        if (sp->pDepthStencilAttachment && sp->pDepthStencilAttachment->attachment < 16 && ls_ro_layout(sp->pDepthStencilAttachment->layout)) r->ro[sp->pDepthStencilAttachment->attachment] = 1;
        for (uint32_t k = 0; k < sp->inputAttachmentCount; k++) if (sp->pInputAttachments[k].attachment < 16) r->role[sp->pInputAttachments[k].attachment] |= 8;
        if (sp->viewMask) ext = 1;
    }
    r->ok = ci->subpassCount == 1 && ci->attachmentCount <= 10 && !ext;
    r->rp2 = 1;
    ls_rp_common(r, pay, id);
    ls_put(id, r);
}

// 変えた render pass を作る（mask：attachment ごとに 3 ビット。下 2 ビットが LOAD の直し 1=CLEAR 2=DONT_CARE・上 1 ビットが STORE を DONT_CARE にする）
static uint64_t ls_variant(uint64_t rpid, uint32_t mask) {
    AcquireSRWLockExclusive(&g_ls_lock);
    struct lsr *r = ls_get_locked(rpid);
    if (!r || r->kind != 3 || !r->ok) { ReleaseSRWLockExclusive(&g_ls_lock); return 0; }
    for (struct lsvar *x = r->var; x; x = x->next)
        if (x->mask == mask) { uint64_t id = x->id; ReleaseSRWLockExclusive(&g_ls_lock); return id; }
    uint64_t id = new_id();
    struct lsvar *x = calloc(1, sizeof *x);
    x->mask = mask; x->id = id; x->next = r->var; r->var = x;
    vr_buf b = { 0 };
    vr_put(&b, r->pay, r->pn);
    memcpy(b.p + 8, &id, 8); // [device][id]...
    for (uint32_t i = 0; i < r->na; i++) {
        uint32_t m = (mask >> (3 * i)) & 7;
        if (!m) continue;
        uint32_t *d = (uint32_t *) (b.p + 24 + i * 36); // flags, format, samples, loadOp, storeOp, stencilLoadOp, stencilStoreOp, initial, final
        uint32_t asp = ls_aspect_of((VkFormat) d[1]);
        // 今 LOAD になっている操作だけを変える（CLEAR や DONT_CARE の操作はそのまま）
        uint32_t lo = (m & 3) == 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : (m & 3) == 2 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : 0xffffffffu;
        if (lo != 0xffffffffu) {
            if ((asp & (LS_ASPECT_COLOR | LS_ASPECT_DEPTH)) && d[3] == VK_ATTACHMENT_LOAD_OP_LOAD) d[3] = lo;
            if ((asp & LS_ASPECT_STENCIL) && d[5] == VK_ATTACHMENT_LOAD_OP_LOAD) d[5] = lo;
        }
        if (m & 4) {
            if ((asp & (LS_ASPECT_COLOR | LS_ASPECT_DEPTH)) && d[4] == VK_ATTACHMENT_STORE_OP_STORE) d[4] = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            if ((asp & LS_ASPECT_STENCIL) && d[6] == VK_ATTACHMENT_STORE_OP_STORE) d[6] = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        // LOAD をやめた attachment の initialLayout は UNDEFINED でなくてよい（中身を持ち越さないだけ）。そのままにする
    }
    uint64_t dev;
    memcpy(&dev, b.p, 8);
    rpc_send(r->rp2 ? VR_CREATE_RENDER_PASS2 : VR_CREATE_RENDER_PASS, &b);
    free(b.p);
    ReleaseSRWLockExclusive(&g_ls_lock);
    return id;
}

static void ls_destroy_rp_variants(VkDevice dv, uint64_t id) {
    if (!ls_on()) return;
    AcquireSRWLockShared(&g_ls_lock);
    struct lsr *r = ls_get_locked(id);
    if (r && r->kind == 3)
        for (struct lsvar *x = r->var; x; x = x->next) send_destroy(VR_DESTROY_RENDER_PASS, dv, x->id);
    ReleaseSRWLockShared(&g_ls_lock);
}

// graphics pipeline が深さ・stencil を使うか（動的に決まる物も、使うと見る）
struct lsp { uint32_t kind; int uses_ds; int blend; int topo; };  // kind=4
static void ls_note_pipes(uint32_t n, const VkGraphicsPipelineCreateInfo *ci, const VkPipeline *out) {
    if (!ls_on()) return;
    for (uint32_t i = 0; i < n; i++) {
        if (!out[i]) continue;
        struct lsp *p = calloc(1, sizeof *p);
        p->kind = 4;
        const VkPipelineDepthStencilStateCreateInfo *d = ci[i].pDepthStencilState;
        int uses = 0;
        if (d) uses = d->depthTestEnable || d->depthWriteEnable || d->stencilTestEnable || d->depthBoundsTestEnable;
        if (ci[i].pDynamicState)
            for (uint32_t k = 0; k < ci[i].pDynamicState->dynamicStateCount; k++) {
                VkDynamicState ds = ci[i].pDynamicState->pDynamicStates[k];
                if (ds == VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE || ds == VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE || ds == VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE ||
                    ds == VK_DYNAMIC_STATE_DEPTH_COMPARE_OP) uses = 1;
            }
        p->uses_ds = uses;
        if (ci[i].pColorBlendState)
            for (uint32_t k = 0; k < ci[i].pColorBlendState->attachmentCount; k++) p->blend |= ci[i].pColorBlendState->pAttachments[k].blendEnable != 0;
        p->topo = ci[i].pInputAssemblyState ? (int) ci[i].pInputAssemblyState->topology : -1;
        ls_put(ID(out[i]), p);
    }
}

// ---- command buffer の中の出来事 ----
enum { LE_RPB = 1, LE_RPE, LE_NEXT, LE_CLRIMG, LE_CLRATT, LE_XDST, LE_XSRC, LE_BAR, LE_BIND, LE_DRAW, LE_UNK, LE_DISP, LE_PIPE, LE_ST, LE_RISK };
struct lsev { uint8_t k; uint32_t x, y, z, w; uint64_t a, b; };

static void ls_ev(struct cb *c, uint8_t k, uint64_t a, uint64_t b, uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    if (!ls_on()) return;
    if (k == LE_DRAW && c->nlse && c->lse[c->nlse - 1].k == LE_DRAW) { c->lse[c->nlse - 1].x++; return; }
    if (c->nlse == c->clse) {
        size_t cap = c->clse ? c->clse * 2 : 256;
        void *p = realloc(c->lse, cap * sizeof *c->lse);
        if (!p) return;
        c->lse = p;
        c->clse = cap;
    }
    c->lse[c->nlse++] = (struct lsev) { k, x, y, z, w, a, b };
}

// 描画の中の vkCmdClearAttachments。first：その render pass の始めから、まだ描いていないか
static void ls_ev_xfer(struct cb *c, VkImage dst, VkImage src) {
    if (dst) ls_ev(c, LE_XDST, ID(dst), 0, 0, 0, 0, 0);
    if (src) ls_ev(c, LE_XSRC, ID(src), 0, 0, 0, 0, 0);
}

// ---- 調べ ----
// 事前に、set と framebuffer が指す画像を出来事ごとに引いておく（毎回表を引くと遅い）
struct lspre { uint8_t n; uint8_t many; uint8_t stor; struct img *im[12]; };

static void lspre_add(struct lspre *p, struct img *im) {
    if (!im) return;
    for (int i = 0; i < p->n; i++) if (p->im[i] == im) return;
    if (p->n >= 12) { p->many = 1; return; }
    p->im[p->n++] = im;
}
static int lspre_has(const struct lspre *p, const struct img *im) {
    if (p->many) return 1;
    for (int i = 0; i < p->n; i++) if (p->im[i] == im) return 1;
    return 0;
}

static void ls_pre_set(struct lspre *p, struct dset *s) {
    if (!s) return;
    for (uint32_t k = 0; k < s->n; k++) {
        uint32_t t = s->e[k].type;
        if (t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || t == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC ||
            t == VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER) p->stor = 1;
        if (!(t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER || t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ||
              t == VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT)) continue;
        if (!s->e[k].h1) continue;
        struct lsv *v = ls_get(s->e[k].h1);
        if (v && v->kind == 1) lspre_add(p, v->im); else if (!v) p->many = 1; // 分からない view は何でも指すと見る
    }
}

// 画像 im を触る出来事か。1 attachment・2 全体を消す・3 一部を書く・4 読む・5 descriptor
static int ls_touch(const struct lsev *e, const struct lspre *pre, const struct img *im) {
    switch (e->k) {
    case LE_CLRIMG: return e->a == ID(im) ? 2 : 0;
    case LE_XDST: return e->a == ID(im) ? 3 : 0;
    case LE_XSRC: return e->a == ID(im) ? 4 : 0;
    case LE_BIND: return lspre_has(pre, im) ? 5 : 0;
    case LE_RPB: return lspre_has(pre, im) ? 1 : 0;
    default: return 0;
    }
}

// from と to の間（両端は含まない）で、画像 im を読む・書くかもしれないか。
// 描画や計算は、それより前に結んだ set も使える（set は render pass をまたいで残る）ので、間に描画があれば、前に結んだ set も見る
static int ls_maybe_used(const struct cb *c, const struct lspre *pre, const struct img *im, size_t from, size_t to) {
    int draws = 0;
    for (size_t k = from + 1; k < to; k++) {
        if (ls_touch(&c->lse[k], &pre[k], im)) return 1;
        if (c->lse[k].k == LE_DRAW || c->lse[k].k == LE_DISP) draws = 1;
    }
    if (!draws) return 0;
    for (size_t k = 0; k <= from && k < to; k++)
        if (c->lse[k].k == LE_BIND && lspre_has(&pre[k], im)) return 1;
    return 0;
}

static const char *ls_opname(uint32_t o, int store) {
    if (store) return o == VK_ATTACHMENT_STORE_OP_STORE ? "STORE" : o == VK_ATTACHMENT_STORE_OP_DONT_CARE ? "dc" : "none";
    return o == VK_ATTACHMENT_LOAD_OP_LOAD ? "LOAD" : o == VK_ATTACHMENT_LOAD_OP_CLEAR ? "CLEAR" : o == VK_ATTACHMENT_LOAD_OP_DONT_CARE ? "dc" : "none";
}

// 画素あたりのバイト数（読み書きの量の目安）
static double ls_bpp(uint32_t f) {
    switch (f) {
    case VK_FORMAT_R8_UNORM: case VK_FORMAT_R8_UINT: case VK_FORMAT_S8_UINT: return 1;
    case VK_FORMAT_R8G8_UNORM: case VK_FORMAT_R16_UNORM: case VK_FORMAT_R16_SFLOAT: case VK_FORMAT_D16_UNORM: case VK_FORMAT_R16_UINT: return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT: case VK_FORMAT_R16G16B16A16_UNORM: case VK_FORMAT_R32G32_SFLOAT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return 8;
    case VK_FORMAT_R32G32B32A32_SFLOAT: return 16;
    default: return 4;
    }
}
// 数え上げ（IPHONE_VK_STATS のときに出す）。候補：直せる物（直すかどうかは別）・直した物
// command buffer の最後の書き戻し（そのあと、この command buffer の中では触らない）が、次に触る command buffer で要らなかったか（見積もりだけ）。
// 要らない＝次の最初の使い方が、全面を CLEAR か DONT_CARE で始める pass か、全面を消す命令
#define LS_PEND 512
static struct { struct img *im; double bytes; int only_att; } g_lspend[LS_PEND];
static struct { double dead_b, live_b, dead_att_b, live_att_b; uint64_t dead_n, live_n, dead_att_n, live_att_n; } g_lscross;
static int ls_att_only(const struct img *im) { return !(im->ls_usage & (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)); }
enum { LC_CLRIMG, LC_CLRPASS, LC_CLRATT, LC_STORE_NEXT_CLEAR, LC_DEPTH_UNUSED, LC_MERGE, LC_N };
static struct {
    uint64_t passes, load_seen, store_seen, cb_n, cb_skip;
    uint64_t cand_n[LC_N], cand_px[LC_N];   // 直せる物の数と画素
    uint64_t done_n[LC_N], done_px[LC_N];   // 実際に直した物
    uint64_t load_all_px, store_all_px;
    uint64_t sw, var_made;
    uint64_t mrej[8], merge_groups, merge_members, merge_done, proc_n;
    double proc_ms;
    double merge_done_b;
    double load_all_b, store_all_b, rm_load_b, rm_store_b;   // 読み書きのバイト数（直す前の合計・直して減った分）
    uint64_t rm_load_n, rm_store_n;
} g_lsst;

struct lsplan {
    size_t ev;          // RPB の出来事の番号
    uint32_t mask;      // attachment ごとに 3 ビット
    uint32_t cvset;     // 消す値を決めた attachment
    VkClearValue cv[10];
    VkRect2D area;
    uint32_t cvc;       // 元の消す値の数
    uint32_t pmask;     // 試験用：書き戻しをやめた attachment を、終わりで壊す（IPHONE_VK_LSPOISON=1）
    struct img *pim[10];
    uint8_t pdepth[10];
};
struct lsedit { size_t off, len; int kind; size_t plan; uint8_t *ins; size_t nins; }; // kind 1：begin の差し替え・2：消す・3：差し込む（len は 0）

static int edit_cmp(const void *a, const void *b) {
    const struct lsedit *x = a, *y = b;
    if (x->off != y->off) return x->off < y->off ? -1 : 1;
    return x->len < y->len ? -1 : x->len > y->len;
}

static int ls_pass_attr(const struct lsr *r, uint32_t a, uint32_t *loads_color_depth, uint32_t *loads_stencil, uint32_t *stores_cd, uint32_t *stores_s) {
    uint32_t asp = ls_aspect_of((VkFormat) r->a[a].fmt);
    *loads_color_depth = (asp & (LS_ASPECT_COLOR | LS_ASPECT_DEPTH)) && r->a[a].load == VK_ATTACHMENT_LOAD_OP_LOAD;
    *loads_stencil = (asp & LS_ASPECT_STENCIL) && r->a[a].sload == VK_ATTACHMENT_LOAD_OP_LOAD;
    *stores_cd = (asp & (LS_ASPECT_COLOR | LS_ASPECT_DEPTH)) && r->a[a].store == VK_ATTACHMENT_STORE_OP_STORE;
    *stores_s = (asp & LS_ASPECT_STENCIL) && r->a[a].sstore == VK_ATTACHMENT_STORE_OP_STORE;
    return (int) asp;
}

// すべての側面（色・深さ・stencil）が、始めに CLEAR か DONT_CARE か（LOAD で前を読まない）
static int ls_no_load(const struct lsr *r, uint32_t a) {
    uint32_t l1, l2, s1, s2;
    ls_pass_attr(r, a, &l1, &l2, &s1, &s2);
    return !l1 && !l2;
}
// すべての側面が CLEAR
static int ls_all_clear(const struct lsr *r, uint32_t a) {
    uint32_t asp = ls_aspect_of((VkFormat) r->a[a].fmt);
    if ((asp & (LS_ASPECT_COLOR | LS_ASPECT_DEPTH)) && r->a[a].load != VK_ATTACHMENT_LOAD_OP_CLEAR) return 0;
    if ((asp & LS_ASPECT_STENCIL) && r->a[a].sload != VK_ATTACHMENT_LOAD_OP_CLEAR) return 0;
    return 1;
}
// いずれかの側面が CLEAR
static int ls_any_clear(const struct lsr *r, uint32_t a) {
    uint32_t asp = ls_aspect_of((VkFormat) r->a[a].fmt);
    if ((asp & (LS_ASPECT_COLOR | LS_ASPECT_DEPTH)) && r->a[a].load == VK_ATTACHMENT_LOAD_OP_CLEAR) return 1;
    if ((asp & LS_ASPECT_STENCIL) && r->a[a].sload == VK_ATTACHMENT_LOAD_OP_CLEAR) return 1;
    return 0;
}

static int ls_img_simple(const struct img *im) { return im && im->ls_mips == 1 && im->ls_layers == 1 && im->ls_samples == 1 && !im->bc; }
static int ls_area_full(const VkRect2D *a, const struct img *im) {
    return a->offset.x == 0 && a->offset.y == 0 && a->extent.width >= im->ls_w && a->extent.height >= im->ls_h;
}

static void ls_count(int cls, int applied, double px) {
    if (applied) { g_lsst.done_n[cls]++; g_lsst.done_px[cls] += (uint64_t) px; }
    else { g_lsst.cand_n[cls]++; g_lsst.cand_px[cls] += (uint64_t) px; }
}


// ---- pass どうしをまとめられる見積もり（直さない。数えるだけ） ----
struct lssum { struct img *w[12]; int nw, manyw; struct img *r[40]; int nr, manyr, stor; };
static void ls_sum(const struct cb *c, const struct lspre *pre, size_t i, size_t j, struct lssum *s) {
    memset(s, 0, sizeof *s);
    s->nw = pre[i].n; s->manyw = pre[i].many;
    memcpy(s->w, pre[i].im, sizeof(struct img *) * pre[i].n);
    for (size_t k = i + 1; k < j; k++) {
        if (c->lse[k].k != LE_BIND) continue;
        if (pre[k].stor) s->stor = 1;
        if (pre[k].many) s->manyr = 1;
        for (int q = 0; q < pre[k].n; q++) {
            int dup = 0;
            for (int z = 0; z < s->nr; z++) dup |= s->r[z] == pre[k].im[q];
            if (dup) continue;
            if (s->nr >= 40) { s->manyr = 1; break; }
            s->r[s->nr++] = pre[k].im[q];
        }
    }
}
static int ls_x(struct img *const *a, int na, int manya, struct img *const *b, int nb, int manyb) {
    if ((manya && (nb || manyb)) || (manyb && (na || manya))) return 1;
    for (int i = 0; i < na; i++) for (int k = 0; k < nb; k++) if (a[i] == b[k]) return 1;
    return 0;
}

static struct { int taken; uint64_t presents, passes; double load_b, store_b, rm_load_b, rm_store_b, merge_b, dead_b, live_b; uint64_t rm_load_n, rm_store_n, load_n, store_n; } g_lssnap;
static void ls_snapshot(void) {
    if (g_lssnap.taken || g_stat.presents < 300) return;
    g_lssnap.taken = 1;
    g_lssnap.presents = g_stat.presents; g_lssnap.passes = g_lsst.passes;
    g_lssnap.load_b = g_lsst.load_all_b; g_lssnap.store_b = g_lsst.store_all_b; g_lssnap.rm_load_b = g_lsst.rm_load_b; g_lssnap.rm_store_b = g_lsst.rm_store_b;
    g_lssnap.merge_b = (double) g_lsst.cand_px[LC_MERGE]; g_lssnap.dead_b = g_lscross.dead_b; g_lssnap.live_b = g_lscross.live_b;
    g_lssnap.rm_load_n = g_lsst.rm_load_n; g_lssnap.rm_store_n = g_lsst.rm_store_n; g_lssnap.load_n = g_lsst.load_seen; g_lssnap.store_n = g_lsst.store_seen;
}


// 間の命令が pipeline barrier（memory barrier と image barrier だけ。buffer の barrier は無し）の並びだけか。
// image barrier の画像を imgs に入れる（layout の変更はその画像を使う pass のために置いてあるので、その pass と一緒に動かさず、
// 位置を変えないでおく。ただし、まとめる pass の画像と重なるなら動かせない）
static int ls_gap_parse(const uint8_t *p, size_t n, struct img **imgs, int *nimg, int max) {
    size_t at = 0;
    *nimg = 0;
    while (at < n) {
        uint32_t op;
        if (n - at < 24) return 0;
        memcpy(&op, p + at, 4);
        if (op != VR_CMD_PIPELINE_BARRIER && op != VR_CMD_PIPELINE_BARRIER2) return 0;
        uint32_t nm, nb, ni = 0;
        memcpy(&nm, p + at + 16, 4);
        if (nm > 16) return 0;
        size_t q = at + 20 + (size_t) nm * 8;
        if (n < q + 4) return 0;
        memcpy(&nb, p + q, 4);
        if (nb) return 0;
        q += 4;
        if (op == VR_CMD_PIPELINE_BARRIER2) {
            if (n < q + 4) return 0;
            memcpy(&ni, p + q, 4);
            q += 4;
            if (ni > 64 || n < q + (size_t) ni * 52) return 0;
            for (uint32_t k = 0; k < ni; k++) {
                uint64_t im;
                uint32_t ol, nw2, sq, dq;
                memcpy(&ol, p + q + (size_t) k * 52 + 8, 4);
                memcpy(&nw2, p + q + (size_t) k * 52 + 12, 4);
                memcpy(&sq, p + q + (size_t) k * 52 + 16, 4);
                memcpy(&dq, p + q + (size_t) k * 52 + 20, 4);
                memcpy(&im, p + q + (size_t) k * 52 + 24, 8);
                if (g_ls_dbg) { static int dbgn; if (dbgn++ < 12 && !(ol == nw2 && sq == dq)) say("[ls] 間の image barrier：画像 %x 古い layout %u 新しい layout %u 持ち主 %u→%u", (unsigned) (im & 0xffffff), ol, nw2, sq, dq); }
                if (ol == nw2 && sq == dq) continue;   // layout も持ち主も変えない（同期だけ）。位置を変えても画像の状態は変わらない
                if (*nimg >= max) return 0;
                imgs[(*nimg)++] = (struct img *) (uintptr_t) im;
            }
            q += (size_t) ni * 52;
        }
        at = q;
    }
    return 1;
}

// gap（barrier の並び）から、画像 drop[] への image barrier を取り除いて out に書く。
// まとめる pass の画像が、間で「attachment の形 → 読む形 → attachment の形」と行って戻るだけの変更は、まとめた pass の中では起きないので要らない
static void ls_gap_strip(const uint8_t *p, size_t n, struct img *const *drop, int ndrop, vr_buf *out) {
    size_t at = 0;
    while (at < n) {
        uint32_t op, nm, nb, ni = 0;
        memcpy(&op, p + at, 4);
        memcpy(&nm, p + at + 16, 4);
        size_t q = at + 20 + (size_t) nm * 8 + 4;   // nb の後ろ
        memcpy(&nb, p + q - 4, 4);
        if (op != VR_CMD_PIPELINE_BARRIER2) {
            vr_put(out, p + at, q - at);
            at = q;
            continue;
        }
        memcpy(&ni, p + q, 4);
        size_t ib = q + 4;
        uint32_t keep = 0;
        for (uint32_t k = 0; k < ni; k++) {
            uint64_t im;
            memcpy(&im, p + ib + (size_t) k * 52 + 24, 8);
            int d = 0;
            for (int z = 0; z < ndrop; z++) d |= (uint64_t) (uintptr_t) drop[z] == im;
            keep += !d;
        }
        uint32_t newop = keep ? VR_CMD_PIPELINE_BARRIER2 : VR_CMD_PIPELINE_BARRIER;
        vr_u32(out, newop);
        vr_put(out, p + at + 4, q - at - 4);        // src・dst・dep・nm・memory barriers・nb
        if (keep) {
            vr_u32(out, keep);
            for (uint32_t k = 0; k < ni; k++) {
                uint64_t im;
                memcpy(&im, p + ib + (size_t) k * 52 + 24, 8);
                int d = 0;
                for (int z = 0; z < ndrop; z++) d |= (uint64_t) (uintptr_t) drop[z] == im;
                if (!d) vr_put(out, p + ib + (size_t) k * 52, 52);
            }
        }
        at = ib + (size_t) ni * 52;
    }
}

static void ls_gap_dump(const uint8_t *p, size_t n) {
    size_t at = 0;
    while (n - at >= 24) {
        uint32_t op, nm, nb, ni = 0;
        memcpy(&op, p + at, 4);
        memcpy(&nm, p + at + 16, 4);
        if (nm > 16) break;
        size_t q = at + 20 + (size_t) nm * 8;
        if (n < q + 4) break;
        memcpy(&nb, p + q, 4);
        q += 4;
        if (op == VR_CMD_PIPELINE_BARRIER2) {
            memcpy(&ni, p + q, 4);
            q += 4;
            for (uint32_t k = 0; k < ni && n >= q + (size_t) (k + 1) * 52; k++) {
                uint32_t ol, nw2;
                uint64_t im;
                memcpy(&ol, p + q + (size_t) k * 52 + 8, 4);
                memcpy(&nw2, p + q + (size_t) k * 52 + 12, 4);
                memcpy(&im, p + q + (size_t) k * 52 + 24, 8);
                fprintf(stderr, " %x:%u>%u", (unsigned) (im & 0xffffff), ol, nw2);
            }
            q += (size_t) ni * 52;
        }
        at = q;
    }
}

// pass の中（from と to の間）で書いた状態の印（LE_ST）と、最初の描画より前に書いた物
static uint64_t ls_slots(const struct cb *c, size_t from, size_t to, uint64_t *pre_draw) {
    uint64_t all = 0, pd = 0;
    int drawn = 0;
    for (size_t k = from + 1; k < to; k++) {
        if (c->lse[k].k == LE_DRAW || c->lse[k].k == LE_CLRATT) drawn = 1;
        if (c->lse[k].k != LE_ST) continue;
        uint64_t b = 1ull << (c->lse[k].x & 63);
        all |= b;
        if (!drawn) pd |= b;
    }
    if (pre_draw) *pre_draw = pd;
    return all;
}
static int ls_has_risk(const struct cb *c, size_t from, size_t to) {
    // 描画の中の occlusion query（begin・end がどちらも同じ pass の中）は、pass をまとめても動かしても結果が変わらないので、危なくない
    for (size_t k = from; k <= to && k < c->nlse; k++) {
        if (c->lse[k].k == LE_UNK) return 1;
        if (c->lse[k].k == LE_RISK && !(c->lse[k].x == 2 || c->lse[k].x == 3 || c->lse[k].x == 7 || c->lse[k].x == 8)) return 1;
    }
    return 0;
}

struct lsgroup { size_t m[8]; int n; size_t first_ev, last_ev; double bytes; struct img *att[12]; int natt; };
static const char *const LS_MREJ[] = { "attachment が違う・形が違う", "間の命令が barrier だけではない", "間の pass を前へ動かせない", "状態を引き継ぐ", "query・geometry・2 次 cb がある", "LOAD・STORE の指定が違う", "ほかの直しと重なる", "間の barrier の画像が group の画像と重なる" };

static void ls_mark_poison(struct lsplan *pl, uint32_t a, struct img *im, int isdepth) {
    if (!g_ls_poison || a >= 10) return;
    pl->pmask |= 1u << a;
    pl->pim[a] = im;
    pl->pdepth[a] = (uint8_t) isdepth;
}

// 1 つの command buffer の render pass を調べ、直す。直した後の命令列を c->s に入れ替える
static void ls_process(struct cb *c) {
    ls_ctl_poll();
    ls_snapshot();
    if (!c->nlse) return;
    int logging = g_ls_log_frame >= 0 && (int) g_stat.presents + 1 == g_ls_log_frame;
    if (logging) fprintf(stderr, "[ls] ==== command buffer %llu・出来事 %zu・presents %llu\n", (unsigned long long) g_lsst.cb_n, c->nlse, (unsigned long long) g_stat.presents);
    uint32_t opt = (uint32_t) g_ls_opt;
    size_t nev = c->nlse;
    g_lsst.cb_n++;
    int unk = 0;
    size_t nrpb = 0;
    for (size_t i = 0; i < nev; i++) { if (c->lse[i].k == LE_UNK) unk = 1; if (c->lse[i].k == LE_RPB) nrpb++; }
    if (!nrpb) return;
    struct lspre *pre = calloc(nev, sizeof *pre);
    for (size_t i = 0; i < nev; i++) {
        const struct lsev *e = &c->lse[i];
        if (e->k == LE_BIND) ls_pre_set(&pre[i], (struct dset *) (uintptr_t) e->a);
        else if (e->k == LE_RPB) {
            struct lsf *f = ls_get(e->b);
            if (!f || f->kind != 2) { pre[i].many = 1; continue; }
            for (uint32_t q = 0; q < f->n; q++) {
                struct lsv *v = ls_get(f->v[q]);
                if (v && v->kind == 1) lspre_add(&pre[i], v->im); else pre[i].many = 1;
            }
        }
    }
    struct lsplan *plan = calloc(nrpb, sizeof *plan);
    size_t *planof = calloc(nev, sizeof *planof);   // RPB の出来事 → plan の番号 + 1
    size_t *endof = calloc(nev, sizeof *endof);     // RPB の出来事 → 対応する RPE の番号
    int *multi = calloc(nev, sizeof *multi);
    int *ndraws = calloc(nev, sizeof *ndraws);
    size_t np = 0;
    for (size_t i = 0; i < nev; i++) {
        if (c->lse[i].k != LE_RPB) continue;
        size_t j = i + 1;
        for (; j < nev && c->lse[j].k != LE_RPE; j++) {
            if (c->lse[j].k == LE_NEXT) multi[i] = 1;
            if (c->lse[j].k == LE_DRAW) ndraws[i] += (int) c->lse[j].x;
            if (c->lse[j].k == LE_CLRATT) ndraws[i] += 100000; // 消す命令が入っている
        }
        endof[i] = j;
        plan[np].ev = i;
        planof[i] = np + 1;
        const uint8_t *bp = c->s.p + c->lse[i].x;
        memcpy(&plan[np].area, bp + 4 + 8 + 8, sizeof(VkRect2D));
        memcpy(&plan[np].cvc, bp + 4 + 8 + 8 + sizeof(VkRect2D), 4);
        np++;
    }
    size_t ndel = 0;
    size_t *delev = calloc(nev, sizeof *delev);
    size_t passno = 0;
    for (size_t i = 0; i < nev; i++) {
        if (c->lse[i].k != LE_RPB) continue;
        size_t j = endof[i];
        struct lsplan *pl = &plan[planof[i] - 1];
        struct lsr *r = ls_get(c->lse[i].a);
        struct lsf *f = ls_get(c->lse[i].b);
        passno++;
        g_lsst.passes++;
        if (!r || !f || r->kind != 3 || f->kind != 2) { if (logging) fprintf(stderr, "[ls] pass %zu：render pass か framebuffer を覚えていない\n", passno); continue; }
        int simple = r->ok && !multi[i] && !unk;
        int ds_unused = 1;
        for (size_t k = i + 1; k < j && ds_unused; k++) {
            if (c->lse[k].k == LE_PIPE) {
                struct lsp *pp = ls_get(c->lse[k].a);
                if (!pp || pp->kind != 4 || pp->uses_ds) ds_unused = 0;
            } else if (c->lse[k].k == LE_CLRATT) ds_unused = 0;
        }
        for (uint32_t a = 0; a < r->na && a < f->n; a++) {
            struct lsv *v = ls_get(f->v[a]);
            if (!v || v->kind != 1 || !v->im) continue;
            struct img *im = v->im;
            const struct lsatt *at = &r->a[a];
            uint32_t ld_cd, ld_s, st_cd, st_s;
            int asp = ls_pass_attr(r, a, &ld_cd, &ld_s, &st_cd, &st_s);
            int isdepth = (asp & (LS_ASPECT_DEPTH | LS_ASPECT_STENCIL)) != 0;
            int loads = ld_cd || ld_s, stores = st_cd || st_s;
            double px = (double) pl->area.extent.width * pl->area.extent.height;
            if (loads) { g_lsst.load_seen++; g_lsst.load_all_px += (uint64_t) px; g_lsst.load_all_b += px * ls_bpp(at->fmt); }
            if (stores) { g_lsst.store_seen++; g_lsst.store_all_px += (uint64_t) px; g_lsst.store_all_b += px * ls_bpp(at->fmt); }
            int okatt = simple && ls_img_simple(im) && (r->role[a] == 1 || r->role[a] == 2) && ls_area_full(&pl->area, im);
            const char *why_l = "", *why_s = "";
            // ---- LOAD を減らす ----
            if (loads && okatt) {
                // 前に何があったか
                size_t pk = (size_t) -1;
                int prevk = 0;
                for (size_t k = i; k-- > 0;) {
                    int t = ls_touch(&c->lse[k], &pre[k], im);
                    if (t) { prevk = t; pk = k; break; }
                }
                if (prevk == 2 && c->lse[pk].y && !ls_maybe_used(c, pre, im, pk, i)) {
                    // (1a) 直前に画像全体を消した。消す値で始める
                    const struct lsev *ce = &c->lse[pk];
                    int dsclear = ce->x == 2;
                    if (dsclear == isdepth && !ls_any_clear(r, a) && !r->ro[a] && at->fmt == im->ls_fmt) {
                        why_l = "画像を消した直後→CLEAR";
                        ls_count(LC_CLRIMG, 0, px);
                        if (opt & 1) {
                            const uint8_t *cp = c->s.p + ce->z;
                            VkClearValue cvv;
                            memset(&cvv, 0, sizeof cvv);
                            if (dsclear) memcpy(&cvv.depthStencil, cp + 4 + 8 + 4, sizeof(VkClearDepthStencilValue));
                            else memcpy(&cvv.color, cp + 4 + 8 + 4, sizeof(VkClearColorValue));
                            pl->mask |= 1u << (3 * a);
                            pl->cvset |= 1u << a;
                            pl->cv[a] = cvv;
                            delev[ndel++] = pk;
                            ls_count(LC_CLRIMG, 1, px);
                        }
                    }
                } else if (prevk == 1 && c->lse[pk].k == LE_RPB && endof[pk] < i && !ls_maybe_used(c, pre, im, endof[pk], i)) {
                    // (1b) 直前の render pass が「消すだけ」の pass（描かない）で、全面を消している。この pass の始めを CLEAR にして、あちらの書き戻しをやめる
                    size_t ka = pk;
                    struct lsr *ra = ls_get(c->lse[ka].a);
                    struct lsf *fa = ls_get(c->lse[ka].b);
                    struct lsplan *pa = &plan[planof[ka] - 1];
                    if (ra && fa && ra->kind == 3 && fa->kind == 2 && ra->ok && !multi[ka] && !unk && ndraws[ka] == 0) {
                        int aa = -1;
                        for (uint32_t q = 0; q < ra->na && q < fa->n; q++) {
                            struct lsv *va = ls_get(fa->v[q]);
                            if (va && va->kind == 1 && va->im == im) { aa = (int) q; break; }
                        }
                        uint32_t cvc_a = pa->cvc;
                        if (aa >= 0 && ra->role[aa] == r->role[a] && ls_all_clear(ra, (uint32_t) aa) && ls_area_full(&pa->area, im) && (uint32_t) aa < cvc_a &&
                            !ls_any_clear(r, a) && !r->ro[a] && ra->a[aa].fmt == at->fmt) {
                            why_l = "消すだけの pass の直後→CLEAR";
                            ls_count(LC_CLRPASS, 0, px);
                            if (opt & 1) {
                                const uint8_t *bpa = c->s.p + c->lse[ka].x;
                                VkClearValue cvv;
                                memcpy(&cvv, bpa + 4 + 8 + 8 + sizeof(VkRect2D) + 4 + (size_t) aa * sizeof(VkClearValue), sizeof cvv);
                                pl->mask |= 1u << (3 * a);
                                pl->cvset |= 1u << a;
                                pl->cv[a] = cvv;
                                pa->mask |= 4u << (3 * aa);
                                ls_mark_poison(pa, (uint32_t) aa, im, isdepth);
                                ls_count(LC_CLRPASS, 1, px);
                            }
                        }
                    }
                }
                // (2) pass の頭で、描く範囲の全部を vkCmdClearAttachments で消している
                if (!why_l[0]) {
                    for (size_t k = i + 1; k < j && c->lse[k].k != LE_DRAW; k++) {
                        if (c->lse[k].k != LE_CLRATT) continue;
                        const uint8_t *cp = c->s.p + c->lse[k].z;
                        uint32_t na2, nr2;
                        memcpy(&na2, cp + 4, 4);
                        if (na2 > 16) continue;
                        const uint8_t *ca = cp + 8;
                        memcpy(&nr2, ca + (size_t) na2 * sizeof(VkClearAttachment), 4);
                        const uint8_t *cr = ca + (size_t) na2 * sizeof(VkClearAttachment) + 4;
                        int covers = 0;
                        for (uint32_t q = 0; q < nr2 && q < 16; q++) {
                            VkClearRect rc;
                            memcpy(&rc, cr + (size_t) q * sizeof rc, sizeof rc);
                            if (rc.baseArrayLayer == 0 && rc.layerCount >= f->layers && rc.rect.offset.x <= pl->area.offset.x && rc.rect.offset.y <= pl->area.offset.y &&
                                (int64_t) rc.rect.offset.x + rc.rect.extent.width >= (int64_t) pl->area.offset.x + pl->area.extent.width &&
                                (int64_t) rc.rect.offset.y + rc.rect.extent.height >= (int64_t) pl->area.offset.y + pl->area.extent.height) covers = 1;
                        }
                        if (!covers) continue;
                        uint32_t got = 0;
                        for (uint32_t q = 0; q < na2; q++) {
                            VkClearAttachment x;
                            memcpy(&x, ca + (size_t) q * sizeof x, sizeof x);
                            if ((x.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) && x.colorAttachment < 8 && r->colref[x.colorAttachment] == a) got |= LS_ASPECT_COLOR;
                            if ((x.aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) && r->dsref == (int) a) got |= x.aspectMask & 6u;
                        }
                        uint32_t need = (ld_cd ? (asp & (LS_ASPECT_COLOR | LS_ASPECT_DEPTH)) : 0) | (ld_s ? LS_ASPECT_STENCIL : 0);
                        if (need && (got & need) == need) {
                            why_l = "pass の頭で全面を消している→LOAD やめる";
                            ls_count(LC_CLRATT, 0, px);
                            if (opt & 2) {
                                pl->mask |= 2u << (3 * a);
                                ls_count(LC_CLRATT, 1, px);
                            }
                            break;
                        }
                    }
                }
            }
            if ((opt & 64) && stores && okatt && !why_s[0]) {   // 試験用の「わざとの間違い」：次に何があっても書き戻しをやめる。壊れた絵を見つけられるかの確かめ
                for (size_t k = j + 1; k < nev; k++)
                    if (ls_touch(&c->lse[k], &pre[k], im)) { pl->mask |= 4u << (3 * a); ls_mark_poison(pl, a, im, isdepth); why_s = "（試験の間違い）"; break; }
            }
            if (isdepth && ds_unused && r->role[a] == 2 && (loads || stores)) {
                double nb = (double) (loads + stores);
                ls_count(LC_DEPTH_UNUSED, 0, px * nb);
                if (!why_l[0] && !why_s[0]) why_l = "深さを使わない pass";
            }
            // ---- STORE を減らす ----
            if (stores && okatt) {
                size_t nk = (size_t) -1;
                int nextk = 0;
                for (size_t k = j + 1; k < nev; k++) {
                    int t = ls_touch(&c->lse[k], &pre[k], im);
                    if (t) { nextk = t; nk = k; break; }
                }
                if (!nextk && !g_ls_nopend) {
                    int slot = -1;
                    for (int s = 0; s < LS_PEND; s++) if (g_lspend[s].im == im) { slot = s; break; }
                    if (slot < 0) for (int s = 0; s < LS_PEND; s++) if (!g_lspend[s].im) { slot = s; break; }
                    if (slot >= 0) { g_lspend[slot].im = im; g_lspend[slot].bytes = px * ls_bpp(at->fmt); g_lspend[slot].only_att = ls_att_only(im); }
                }
                if (nextk == 2 && c->lse[nk].y && !ls_maybe_used(c, pre, im, j, nk)) {
                    why_s = "次に画像全体を消す→STORE やめる";
                    ls_count(LC_STORE_NEXT_CLEAR, 0, px);
                    if (opt & 4) { pl->mask |= 4u << (3 * a); ls_mark_poison(pl, a, im, isdepth); ls_count(LC_STORE_NEXT_CLEAR, 1, px); }
                } else if (nextk == 1 && c->lse[nk].k == LE_RPB && !ls_maybe_used(c, pre, im, j, nk)) {
                    struct lsr *rn = ls_get(c->lse[nk].a);
                    struct lsf *fn = ls_get(c->lse[nk].b);
                    struct lsplan *pn = &plan[planof[nk] - 1];
                    if (rn && fn && rn->kind == 3 && fn->kind == 2 && rn->ok && !multi[nk]) {
                        int na2 = -1;
                        for (uint32_t q = 0; q < rn->na && q < fn->n; q++) {
                            struct lsv *vn = ls_get(fn->v[q]);
                            if (vn && vn->kind == 1 && vn->im == im) { na2 = (int) q; break; }
                        }
                        if (na2 >= 0 && (rn->role[na2] == 1 || rn->role[na2] == 2) && ls_no_load(rn, (uint32_t) na2) && ls_area_full(&pn->area, im)) {
                            why_s = "次の pass が読み込まずに始まる→STORE やめる";
                            ls_count(LC_STORE_NEXT_CLEAR, 0, px);
                            if (opt & 4) { pl->mask |= 4u << (3 * a); ls_mark_poison(pl, a, im, isdepth); ls_count(LC_STORE_NEXT_CLEAR, 1, px); }
                        }
                    }
                }
            }
            if (logging) {
                unsigned seen[40]; int ns = 0;
                fprintf(stderr, "[ls]   pass %zu att%u 画像 %x 内で読む：", passno, a, (unsigned) ((uintptr_t) im & 0xffffff));
                for (size_t k = i + 1; k < j; k++)
                    if (c->lse[k].k == LE_BIND) for (int q = 0; q < pre[k].n; q++) {
                        unsigned id = (unsigned) ((uintptr_t) pre[k].im[q] & 0xffffff);
                        int dup = 0;
                        for (int z = 0; z < ns; z++) dup |= seen[z] == id;
                        if (!dup && ns < 40) { seen[ns++] = id; fprintf(stderr, "%x ", id); }
                    }
                fprintf(stderr, "\n");
            }
            if (logging) {
                size_t pe = 0;
                for (size_t k = i; k-- > 0;) if (c->lse[k].k == LE_RPE) { pe = c->lse[k].x + 4; break; }
                fprintf(stderr, "[ls] 隙間 %zu バイト：", (size_t) c->lse[i].x - pe);
                if (c->lse[i].x > pe) ls_gap_dump(c->s.p + pe, c->lse[i].x - pe);
                fprintf(stderr, "\n");
            }
            if (logging)
                fprintf(stderr, "[ls] pass %zu(draw %d%s) att%u %s fmt=%u %ux%u 読み込み=%s 書き戻し=%s 使い道=%u | 読み込み：%s | 書き戻し：%s\n", passno, ndraws[i] % 100000,
                        multi[i] ? "・subpass 複数" : "", a, isdepth ? "深さ" : "色", at->fmt, im->ls_w, im->ls_h, ls_opname(at->load, 0), ls_opname(at->store, 1), r->role[a],
                        why_l, why_s);
        }
    }
    // 同じ attachment の pass どうしをまとめる（間の pass を前へ動かして 1 つの pass にする）。LOAD と STORE が 1 組ずつ減る
    struct lsgroup *groups = calloc(nrpb, sizeof *groups);
    size_t ngroups = 0;
    char *used = calloc(nev, 1);   // すでにどれかの group に入った RPB
    for (size_t i = 0; i < nev; i++) {
        if (c->lse[i].k != LE_RPB || multi[i] || unk || used[i]) continue;
        struct lsr *ra = ls_get(c->lse[i].a);
        struct lsf *fa = ls_get(c->lse[i].b);
        if (!ra || !fa || ra->kind != 3 || fa->kind != 2 || !ra->ok) continue;
        struct lsplan *pa = &plan[planof[i] - 1];
        if (pa->mask) continue;
        struct lssum sg;       // group 全体の読む物・書く物
        ls_sum(c, pre, i, endof[i], &sg);
        if (sg.stor || sg.manyw || sg.manyr) continue;
        struct lsgroup g;
        memset(&g, 0, sizeof g);
        g.m[g.n++] = i;
        uint64_t wmembers = ls_slots(c, i, endof[i], NULL);   // 入った pass が書いた状態
        uint64_t mafter = 0;                                   // 前の入った pass のあとにある間の pass が書いた状態
        size_t prev_end = endof[i];                            // 前の block の終わり（RPE の出来事）
        int stop = 0;
        for (size_t k = endof[i] + 1; k < nev && !stop; k++) {
            const struct lsev *e = &c->lse[k];
            if (e->k != LE_RPB) {
                if (e->k == LE_RPE) continue;   // 上で飛ばし済みのはず
                stop = 1;   // pass の外の BIND・消す・写し・計算など
                continue;
            }
            size_t kj = endof[k];
            // 前の block との間は barrier だけか
            size_t gs = c->lse[prev_end].x + 4, ge = e->x;
            struct img *gimg[64];
            int ngimg = 0;
            if (ge < gs || !ls_gap_parse(c->s.p + gs, ge - gs, gimg, &ngimg, 64)) {
                g_lsst.mrej[1]++;
                break;
            }
            {
                int bad = 0;
                for (int z = 0; z < ngimg && !bad; z++) {
                    int inw = 0, inr = 0;
                    for (int y = 0; y < sg.nw; y++) inw |= sg.w[y] == gimg[z];
                    for (int y = 0; y < sg.nr; y++) inr |= sg.r[y] == gimg[z];
                    if (!inw && sg.manyw && 0) inw = 1;
                    if (inr && !inw) bad = 1;      // まとめる pass が読む画像の layout が間で変わる
                }
                if (bad || sg.manyr) { g_lsst.mrej[7]++; break; }
            }
            struct lsr *rb = ls_get(e->a);
            struct lsf *fb = ls_get(e->b);
            struct lsplan *pb = &plan[planof[k] - 1];
            if (!rb || !fb || rb->kind != 3 || fb->kind != 2) break;
            if (ls_has_risk(c, k, kj)) {
                g_lsst.mrej[4]++;
                if (g_ls_dbg && g_lsst.mrej[4] < 5) for (size_t z = k; z <= kj; z++) if (c->lse[z].k == LE_RISK || c->lse[z].k == LE_UNK) say("[ls] 危ない命令 種類 %d 番号 %u（pass 出来事 %zu）", c->lse[z].k, c->lse[z].x, k);
                break;
            }
            struct lssum sm;
            ls_sum(c, pre, k, kj, &sm);
            uint64_t pd = 0, wb = ls_slots(c, k, kj, &pd);
            // この pass が、group の attachment と全く同じ使い方をするか
            int same = rb->ok && !multi[k] && !pb->mask && rb->na == ra->na && fb->n == fa->n && pb->area.offset.x == pa->area.offset.x && pb->area.offset.y == pa->area.offset.y &&
                       pb->area.extent.width == pa->area.extent.width && pb->area.extent.height == pa->area.extent.height && fb->w == fa->w && fb->h == fa->h && fb->layers == fa->layers;
            for (uint32_t q = 0; same && q < fa->n; q++) {
                struct lsv *va = ls_get(fa->v[q]), *vb = ls_get(fb->v[q]);
                if (!va || !vb || va->kind != 1 || vb->kind != 1 || va->im != vb->im || va->base_mip != vb->base_mip || va->base_layer != vb->base_layer) same = 0;
            }
            int opsok = 1;
            if (same) {
                // 形・layout・LOAD と STORE の指定・subpass と dependency の中身が同じか
                for (uint32_t q = 0; q < ra->na; q++) {
                    const struct lsatt *x = &ra->a[q], *y = &rb->a[q];
                    uint32_t asp = ls_aspect_of((VkFormat) x->fmt);
                    if (x->fmt != y->fmt || x->samples != y->samples || x->fl != y->fl || ra->role[q] != rb->role[q]) same = 0;
                    if (rb->role[q] != 1 && rb->role[q] != 2) same = 0;
                    if (rb->ro[q] != ra->ro[q]) same = 0;
                    if (x->store != y->store || x->sstore != y->sstore) opsok = 0;
                    if ((asp & (LS_ASPECT_COLOR | LS_ASPECT_DEPTH)) && y->load != VK_ATTACHMENT_LOAD_OP_LOAD) opsok = 0;
                    if ((asp & LS_ASPECT_STENCIL) && y->sload != VK_ATTACHMENT_LOAD_OP_LOAD) opsok = 0;
                }
                size_t off = 24 + (size_t) ra->na * 36;
                if (ra->pn != rb->pn || ra->pn < off || memcmp(ra->pay + off, rb->pay + off, ra->pn - off)) same = 0;
            }
            if (same) {
                if (!opsok) { g_lsst.mrej[5]++; break; }
                if (sm.stor) { g_lsst.mrej[2]++; break; }
                if ((mafter & wmembers & ~pd) && !(opt & 128)) { g_lsst.mrej[3]++; break; }   // この pass が最初の描画の前に書き直さない状態を、間の pass が書き換えている
                if (g.n >= 8) break;
                g.m[g.n++] = k;
                for (int q = 0; q < sm.nr && sg.nr < 40; q++) {
                    int dup = 0;
                    for (int z = 0; z < sg.nr; z++) dup |= sg.r[z] == sm.r[q];
                    if (!dup) sg.r[sg.nr++] = sm.r[q];
                }
                wmembers |= wb;
                mafter = 0;
                g.last_ev = k;
                prev_end = kj;
                k = kj;
                continue;
            }
            if (g.n && !same && (rb->na == ra->na && fb->n == fa->n)) { /* attachment 数は同じでも別の画像：ふつうの間の pass として扱う */ }
            // 間の pass：前（group の前）へ動かせるか
            if (sm.stor || sm.manyw || sm.manyr || sm.nr >= 40) { g_lsst.mrej[2]++; break; }
            if ((ls_x(sm.r, sm.nr, sm.manyr, sg.w, sg.nw, sg.manyw) || ls_x(sm.w, sm.nw, 0, sg.r, sg.nr, sg.manyr) || ls_x(sm.w, sm.nw, 0, sg.w, sg.nw, 0)) && !(opt & 128)) { g_lsst.mrej[2]++; break; }
            mafter |= wb;
            prev_end = kj;
            k = kj;
        }
        if (g.n < 2) continue;
        g.first_ev = i;
        g.natt = sg.nw < 12 ? sg.nw : 12;
        memcpy(g.att, sg.w, sizeof(struct img *) * (size_t) g.natt);
        // group が覚える範囲：他の直し（LOAD・STORE を変える・消す）と重ならない
        int clash = 0;
        for (size_t k = i; k <= endof[g.m[g.n - 1]] && k < nev; k++) {
            if (c->lse[k].k == LE_RPB && plan[planof[k] - 1].mask) clash = 1;
            if (c->lse[k].k == LE_CLRIMG) clash = 1;
        }
        if (clash) { g_lsst.mrej[6]++; continue; }
        // 減る量：メンバー（2 つ目から）が LOAD する attachment と、前の STORE
        double bytes = 0;
        for (int mi = 1; mi < g.n; mi++) {
            struct lsr *rx = ls_get(c->lse[g.m[mi]].a);
            struct lsplan *px2 = &plan[planof[g.m[mi]] - 1];
            for (uint32_t q = 0; rx && q < rx->na; q++) bytes += 2.0 * px2->area.extent.width * px2->area.extent.height * ls_bpp(rx->a[q].fmt);
        }
        g.bytes = bytes;
        ls_count(LC_MERGE, 0, bytes);
        g_lsst.merge_groups++;
        g_lsst.merge_members += (uint64_t) g.n;
        if (logging) fprintf(stderr, "[ls] まとめ：pass %d 個（出来事 %zu〜%zu）・減る LOAD と STORE %.1f MB\n", g.n, g.first_ev, g.last_ev, bytes / 1048576.0);
        for (int mi = 0; mi < g.n; mi++) used[g.m[mi]] = 1;
        if (opt & 16) groups[ngroups++] = g;
        i = endof[g.m[g.n - 1]];
    }
    free(used);
    // 差し替える所を集める
    if (opt) {
        struct lsedit *ed = calloc(np * 2 + ndel + ngroups + 1, sizeof *ed);
        size_t ne = 0;
        for (size_t q = 0; q < np; q++) {
            if (!plan[q].mask) continue;
            struct lsev *e = &c->lse[plan[q].ev];
            ed[ne++] = (struct lsedit) { e->x, e->y - e->x, 1, q };
        }
        for (size_t q = 0; q < np; q++) {
            if (!plan[q].pmask || !plan[q].mask) continue;
            vr_buf pb = { 0 };
            for (uint32_t a = 0; a < 10; a++) {
                if (!(plan[q].pmask >> a & 1)) continue;
                struct img *pim = plan[q].pim[a];
                uint32_t asp = ls_aspect_of((VkFormat) pim->ls_fmt);
                if (plan[q].pdepth[a]) {
                    VkClearDepthStencilValue dv2 = { 0.1234f, 0x55 };
                    VkImageSubresourceRange rg = { asp, 0, 1, 0, 1 };
                    vr_u32(&pb, VR_CMD_CLEAR_DEPTH_STENCIL_IMAGE); vr_u64(&pb, ID(pim)); vr_u32(&pb, VK_IMAGE_LAYOUT_GENERAL);
                    vr_put(&pb, &dv2, sizeof dv2); vr_u32(&pb, 1); vr_put(&pb, &rg, sizeof rg);
                } else {
                    VkClearColorValue col = { .float32 = { 1.f, 0.f, 1.f, 1.f } };
                    VkImageSubresourceRange rg = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                    vr_u32(&pb, VR_CMD_CLEAR_COLOR_IMAGE); vr_u64(&pb, ID(pim)); vr_u32(&pb, VK_IMAGE_LAYOUT_GENERAL);
                    vr_put(&pb, &col, sizeof col); vr_u32(&pb, 1); vr_put(&pb, &rg, sizeof rg);
                }
            }
            ed[ne++] = (struct lsedit) { c->lse[endof[plan[q].ev]].x + 4, 0, 3, 0, pb.p, pb.n };
        }
        for (size_t q = 0; q < ngroups; q++) {
            const struct lsgroup *g = &groups[q];
            size_t last = g->m[g->n - 1];
            size_t rs = c->lse[g->m[0]].x, re = c->lse[endof[last]].x + 4;
            vr_buf pb = { 0 };
            // 区間の中の block を順に見て、入った pass だけ抜く（間の barrier はそのまま。間の pass は残す）
            size_t pos = rs;
            for (size_t k = g->m[0]; k < nev && k <= last; k++) {
                if (c->lse[k].k != LE_RPB) continue;
                int member = 0;
                for (int mi = 0; mi < g->n; mi++) member |= g->m[mi] == k;
                size_t bs = c->lse[k].x, be = c->lse[endof[k]].x + 4;
                if (bs > pos) ls_gap_strip(c->s.p + pos, bs - pos, g->att, g->natt, &pb);
                if (!member) vr_put(&pb, c->s.p + bs, be - bs);
                pos = be;
            }
            if (re > pos) ls_gap_strip(c->s.p + pos, re - pos, g->att, g->natt, &pb);
            // まとめた pass：最初の pass の始まりと中身・ほかの pass の中身・終わり
            size_t a0 = c->lse[g->m[0]].x, a1 = c->lse[endof[g->m[0]]].x;
            vr_put(&pb, c->s.p + a0, a1 - a0);
            for (int mi = 1; mi < g->n; mi++) {
                size_t y0 = c->lse[g->m[mi]].y, y1 = c->lse[endof[g->m[mi]]].x;
                vr_put(&pb, c->s.p + y0, y1 - y0);
            }
            vr_put(&pb, c->s.p + a1, 4);
            if (g_ls_dbg) say("[ls] group %d 個 区間 %zu〜%zu（%zu バイト）を %zu バイトに置き換え", g->n, rs, re, re - rs, pb.n);
            ed[ne++] = (struct lsedit) { rs, re - rs, 4, 0, pb.p, pb.n };
            g_lsst.merge_done += (uint64_t) (g->n - 1);
            g_lsst.merge_done_b += g->bytes;
        }
        for (size_t q = 0; q < ndel; q++) {
            const struct lsev *e = &c->lse[delev[q]];
            // 画像を消す命令の長さ：op・image・layout・値・数・範囲の並び
            size_t len = 4 + 8 + 4 + (e->x == 2 ? sizeof(VkClearDepthStencilValue) : sizeof(VkClearColorValue)) + 4 + (size_t) e->w * sizeof(VkImageSubresourceRange);
            ed[ne++] = (struct lsedit) { e->z, len, 2, 0 };
        }
        if (ne) {
            qsort(ed, ne, sizeof *ed, edit_cmp);
            vr_buf nb = { 0 };
            size_t at = 0;
            int ok = 1;
            for (size_t q = 0; q < ne && ok; q++) {
                if (ed[q].off < at) { ok = 0; break; }
                vr_put(&nb, c->s.p + at, ed[q].off - at);
                at = ed[q].off + ed[q].len;
                if (ed[q].kind == 2) continue;
                if (ed[q].kind == 3) { vr_put(&nb, ed[q].ins, ed[q].nins); continue; }
                if (ed[q].kind == 4) { vr_put(&nb, ed[q].ins, ed[q].nins); continue; }
                struct lsplan *pl = &plan[ed[q].plan];
                const struct lsev *e = &c->lse[pl->ev];
                uint64_t newrp = ls_variant(e->a, pl->mask);
                if (!newrp) { vr_put(&nb, c->s.p + ed[q].off, ed[q].len); continue; }
                const uint8_t *bp = c->s.p + e->x;
                uint32_t cvc = pl->cvc, ncv = cvc;
                for (uint32_t a = 0; a < 10; a++) if ((pl->cvset >> a & 1) && a + 1 > ncv) ncv = a + 1;
                vr_u32(&nb, VR_CMD_BEGIN_RENDER_PASS);
                vr_u64(&nb, newrp);
                vr_put(&nb, bp + 4 + 8, 8);                       // framebuffer
                vr_put(&nb, bp + 4 + 8 + 8, sizeof(VkRect2D));    // 範囲
                vr_u32(&nb, ncv);
                for (uint32_t a = 0; a < ncv; a++) {
                    VkClearValue cv;
                    memset(&cv, 0, sizeof cv);
                    if (a < cvc) memcpy(&cv, bp + 4 + 8 + 8 + sizeof(VkRect2D) + 4 + (size_t) a * sizeof cv, sizeof cv);
                    if (a < 10 && (pl->cvset >> a & 1)) cv = pl->cv[a];
                    vr_put(&nb, &cv, sizeof cv);
                }
                vr_u32(&nb, VK_SUBPASS_CONTENTS_INLINE);
                g_lsst.sw++;
                {   // 減った読み書き
                    struct lsr *rr = ls_get(e->a);
                    for (uint32_t a = 0; rr && a < rr->na && a < 10; a++) {
                        uint32_t m = (pl->mask >> (3 * a)) & 7;
                        uint32_t l1, l2, s1, s2;
                        ls_pass_attr(rr, a, &l1, &l2, &s1, &s2);
                        double bb = (double) pl->area.extent.width * pl->area.extent.height * ls_bpp(rr->a[a].fmt);
                        if ((m & 3) && (l1 || l2)) { g_lsst.rm_load_n++; g_lsst.rm_load_b += bb; }
                        if ((m & 4) && (s1 || s2)) { g_lsst.rm_store_n++; g_lsst.rm_store_b += bb; }
                    }
                }
                if (g_ls_dbg) say("[ls] render pass %llx を %llx に差し替え（mask 0x%x）", (unsigned long long) e->a, (unsigned long long) newrp, pl->mask);
            }
            if (ok) {
                vr_put(&nb, c->s.p + at, c->s.n - at);
                free(c->s.p);
                c->s = nb;
            } else {
                free(nb.p);
                say("[ls] 差し替えの場所が重なった（直さない）");
            }
        }
        for (size_t q = 0; q < ne; q++) free(ed[q].ins);
        free(ed);
    }
    free(groups);
    free(delev);
    free(ndraws);
    free(multi);
    free(endof);
    free(planof);
    free(plan);
    free(pre);
}

static void ls_print(void) {
    static const char *nm[LC_N] = { "直前に画像全体を消している→CLEAR で始める", "直前の「消すだけ」の pass→CLEAR で始める＋あちらの STORE をやめる", "pass の頭で全面を消している→LOAD をやめる", "次に全部書き直す→STORE をやめる", "深さを使わない pass の深さ（LOAD と STORE の合計の画素。直すなら LOAD_OP_NONE が要る）", "同じ attachment の pass どうしを 1 つにまとめる（間の pass を前に動かす）" };
    fprintf(stderr, "  [ls] command buffer %llu・render pass %llu・LOAD の attachment %llu（%.1f M 画素）・STORE の attachment %llu（%.1f M 画素）\n", (unsigned long long) g_lsst.cb_n,
            (unsigned long long) g_lsst.passes, (unsigned long long) g_lsst.load_seen, g_lsst.load_all_px / 1e6, (unsigned long long) g_lsst.store_seen, g_lsst.store_all_px / 1e6);
    for (int k = 0; k < LC_N; k++)
        if (k == LC_MERGE)
            fprintf(stderr, "  [ls] %s：候補 %llu（%.1f MB・1 コマ %.1f MB）\n", nm[k], (unsigned long long) g_lsst.cand_n[k], g_lsst.cand_px[k] / 1048576.0,
                    g_lsst.cand_px[k] / 1048576.0 / (g_stat.presents ? (double) g_stat.presents : 1));
        else
        fprintf(stderr, "  [ls] %s：候補 %llu（%.1f M 画素）・直した %llu（%.1f M 画素）\n", nm[k], (unsigned long long) g_lsst.cand_n[k], g_lsst.cand_px[k] / 1e6,
                (unsigned long long) g_lsst.done_n[k], g_lsst.done_px[k] / 1e6);
    fprintf(stderr, "  [ls] 最後の書き戻し（そのあとこの command buffer では触らない物）が次に触る command buffer で要らなかったか：要らなかった %llu 個 %.1f MB（うち描く先専用の画像 %llu 個 %.1f MB）・要った %llu 個 %.1f MB（うち描く先専用 %llu 個 %.1f MB）\n",
            (unsigned long long) g_lscross.dead_n, g_lscross.dead_b / 1048576.0, (unsigned long long) g_lscross.dead_att_n, g_lscross.dead_att_b / 1048576.0,
            (unsigned long long) g_lscross.live_n, g_lscross.live_b / 1048576.0, (unsigned long long) g_lscross.live_att_n, g_lscross.live_att_b / 1048576.0);
    fprintf(stderr, "  [ls] まとめ：group %llu 個（pass %llu 個）・直した pass %llu 個 %.1f MB・見送った理由：", (unsigned long long) g_lsst.merge_groups, (unsigned long long) g_lsst.merge_members,
            (unsigned long long) g_lsst.merge_done, g_lsst.merge_done_b / 1048576.0);
    for (int k = 0; k < 8; k++) fprintf(stderr, "%s %llu・", LS_MREJ[k], (unsigned long long) g_lsst.mrej[k]);
    fprintf(stderr, "\n");
    if (g_lssnap.taken && g_stat.presents > g_lssnap.presents) {
        double n = (double) (g_stat.presents - g_lssnap.presents), MIBv = 1048576.0;
        fprintf(stderr, "  [ls] 読み込み始め（present %llu）から終わりまでの 1 コマ平均（%.0f コマ）：render pass %.1f 回・LOAD %.1f 個 %.1f MB・STORE %.1f 個 %.1f MB・直して減った LOAD %.1f 個 %.2f MB・STORE %.1f 個 %.2f MB・"
                        "まとめの見積もり %.1f MB・最後の書き戻しで要らなかった物の見積もり %.2f MB（要った物 %.1f MB）\n",
                (unsigned long long) g_lssnap.presents, n, (g_lsst.passes - g_lssnap.passes) / n, (g_lsst.load_seen - g_lssnap.load_n) / n, (g_lsst.load_all_b - g_lssnap.load_b) / n / MIBv,
                (g_lsst.store_seen - g_lssnap.store_n) / n, (g_lsst.store_all_b - g_lssnap.store_b) / n / MIBv, (g_lsst.rm_load_n - g_lssnap.rm_load_n) / n,
                (g_lsst.rm_load_b - g_lssnap.rm_load_b) / n / MIBv, (g_lsst.rm_store_n - g_lssnap.rm_store_n) / n, (g_lsst.rm_store_b - g_lssnap.rm_store_b) / n / MIBv,
                ((double) g_lsst.cand_px[LC_MERGE] - g_lssnap.merge_b) / n / MIBv, (g_lscross.dead_b - g_lssnap.dead_b) / n / MIBv, (g_lscross.live_b - g_lssnap.live_b) / n / MIBv);
    }
    fprintf(stderr, "  [ls] 調べて直すのにかかった時間：command buffer %llu 個で合計 %.1f ms（1 個 %.3f ms・1 コマ %.2f ms）\n", (unsigned long long) g_lsst.proc_n, g_lsst.proc_ms, g_lsst.proc_n ? g_lsst.proc_ms / (double) g_lsst.proc_n : 0.0, g_stat.presents ? g_lsst.proc_ms / (double) g_stat.presents : 0.0);
    {
        double per = g_stat.presents ? (double) g_stat.presents : 1;
        fprintf(stderr, "  [ls] render pass の差し替え %llu 回・減った LOAD %llu 個 %.1f MB・減った STORE %llu 個 %.1f MB（直す前の合計 LOAD %.1f MB・STORE %.1f MB）\n", (unsigned long long) g_lsst.sw,
                (unsigned long long) g_lsst.rm_load_n, g_lsst.rm_load_b / 1048576.0, (unsigned long long) g_lsst.rm_store_n, g_lsst.rm_store_b / 1048576.0,
                g_lsst.load_all_b / 1048576.0, g_lsst.store_all_b / 1048576.0);
        fprintf(stderr, "  [ls] 1 コマあたり（present %llu 回）：LOAD %.1f MB・STORE %.1f MB・直して減った LOAD %.2f MB・STORE %.2f MB\n", (unsigned long long) g_stat.presents,
                g_lsst.load_all_b / per / 1048576.0, g_lsst.store_all_b / per / 1048576.0, g_lsst.rm_load_b / per / 1048576.0, g_lsst.rm_store_b / per / 1048576.0);
    }
}
