// Linux の窓（X11）。icd.c の中に取り込む（#ifndef _WIN32・「窓（surface）」の所）。
// VK_KHR_xcb_surface と VK_KHR_xlib_surface の surface を作り、swapchain が使う細い関数（surface_hwnd・win_size・win_draw・
// jdec_*・monitor_hz・move_to_monitor）を Win32 の物と同じ形で用意する。HWND は、ここでは描く窓（struct xwin）を指す。
//
//   描く口：自分で $DISPLAY に xcb でつなぎ、xcb_put_image で描く。アプリの口（Xlib・xcb）には何も送らない
//           （アプリの糸と取り合わない。窓が先に消えたときの BadWindow などの誤りが、アプリの誤りの処理に届いて止まらない）。
//           自分の口でその窓が見えないとき（$DISPLAY が違う）だけ、アプリの xcb の口（Xlib なら XGetXCBConnection）で描く。
//   X の関数：libxcb.so.1 を初めて窓に描くときに dlopen する。組むときに X の開発用ヘッダが要らず、窓の無いアプリでは読まない。
//   窓の深さ：24 と 32 ビット（1 画素 4 バイトの B・G・R・X）。窓の大きさが絵と違えば、最近傍で窓いっぱいに伸ばす。
//   JPEG：libturbojpeg.so.0 があればそれで（SIMD で速い）、無ければ取り込んだ stb_image で。IPHONE_VK_JPEG_DEC=stb で必ず stb
//   書き換えの速さ：IPHONE_VK_HZ（標準 60。Steam Deck の OLED なら 90）

// ---- xcb（使う物だけ。形は xcb/xcb.h と同じ） ----
typedef struct { unsigned int sequence; } xcbv_cookie;
typedef struct { unsigned int sequence; } xcbg_cookie;
typedef struct {
    uint8_t response_type, depth;
    uint16_t sequence;
    uint32_t length, root;
    int16_t x, y;
    uint16_t width, height, border_width;
    uint8_t pad0[2];
} xcbg_reply;
static struct {
    int tried, ok;
    xcb_connection_t *(*connect)(const char *, int *);
    int (*has_error)(xcb_connection_t *);
    void (*disconnect)(xcb_connection_t *);
    uint32_t (*generate_id)(xcb_connection_t *);
    xcbv_cookie (*create_gc)(xcb_connection_t *, uint32_t, uint32_t, uint32_t, const void *);
    xcbv_cookie (*free_gc)(xcb_connection_t *, uint32_t);
    xcbv_cookie (*put_image)(xcb_connection_t *, uint8_t, uint32_t, uint32_t, uint16_t, uint16_t, int16_t, int16_t, uint8_t, uint8_t, uint32_t, const uint8_t *);
    xcbg_cookie (*get_geometry)(xcb_connection_t *, uint32_t);
    xcbg_reply *(*get_geometry_reply)(xcb_connection_t *, xcbg_cookie, void **);
    uint32_t (*max_req)(xcb_connection_t *);
    int (*flush)(xcb_connection_t *);
    void *(*poll_for_event)(xcb_connection_t *);
} g_xcb;
static SRWLOCK g_xcb_lock = SRWLOCK_INIT;

static int xcb_load(void) {
    AcquireSRWLockExclusive(&g_xcb_lock);
    if (!g_xcb.tried) {
        g_xcb.tried = 1;
        void *h = dlopen("libxcb.so.1", RTLD_NOW | RTLD_LOCAL);
        if (h) {
#define XS(f, n) (*(void **) &g_xcb.f = dlsym(h, n))
            int ok = XS(connect, "xcb_connect") && XS(has_error, "xcb_connection_has_error") && XS(disconnect, "xcb_disconnect") &&
                     XS(generate_id, "xcb_generate_id") && XS(create_gc, "xcb_create_gc") && XS(free_gc, "xcb_free_gc") &&
                     XS(put_image, "xcb_put_image") && XS(get_geometry, "xcb_get_geometry") && XS(get_geometry_reply, "xcb_get_geometry_reply") &&
                     XS(max_req, "xcb_get_maximum_request_length") && XS(flush, "xcb_flush") && XS(poll_for_event, "xcb_poll_for_event");
#undef XS
            g_xcb.ok = ok;
        }
        if (!g_xcb.ok) fprintf(stderr, "[iphone-vk] libxcb.so.1 を読めないので、窓に描けない\n");
    }
    ReleaseSRWLockExclusive(&g_xcb_lock);
    return g_xcb.ok;
}

// ---- 描く窓 ----
struct xwin {
    SRWLOCK lk;            // 描く糸とアプリの糸（大きさを聞く）が同じ物を使う
    xcb_connection_t *c;   // 描く口
    int own;               // 自分で開いた口（片づけるときに閉じる）
    uint32_t win, gc;
    uint8_t depth;
    uint32_t maxbytes;     // 1 回の put_image で送れるバイト
    uint8_t *tmp;          // 窓の大きさに伸ばした絵
    size_t ntmp;
    int warned, dead;
};

// 自分の口で、その窓の大きさと深さを聞く。見えなければ 0
static int xw_geometry(xcb_connection_t *c, uint32_t win, uint32_t *w, uint32_t *h, uint8_t *depth) {
    void *err = NULL;
    xcbg_reply *r = g_xcb.get_geometry_reply(c, g_xcb.get_geometry(c, win), &err);
    free(err);
    if (!r) return 0;
    *w = r->width, *h = r->height;
    if (depth) *depth = r->depth;
    free(r);
    return 1;
}

// アプリの Display の名前（libX11 が読み込まれていれば XDisplayString で聞く）
static const char *xlib_display_name(Display *dpy) {
    void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_NOLOAD);
    const char *(*ds)(Display *) = x11 ? (const char *(*)(Display *)) dlsym(x11, "XDisplayString") : NULL;
    const char *n = ds && dpy ? ds(dpy) : NULL;
    if (x11) dlclose(x11);
    return n;
}

// Xlib の Display から xcb の口を借りる（libX11-xcb が読み込まれていれば）
static xcb_connection_t *xlib_xcb(Display *dpy) {
    void *xx = dlopen("libX11-xcb.so.1", RTLD_NOW | RTLD_NOLOAD);
    xcb_connection_t *(*f)(Display *) = xx ? (xcb_connection_t *(*)(Display *)) dlsym(xx, "XGetXCBConnection") : NULL;
    xcb_connection_t *c = f && dpy ? f(dpy) : NULL;
    if (xx) dlclose(xx);
    return c;
}

static struct xwin *xw_open(xcb_connection_t *appc, Display *dpy, uint32_t win) {
    if (!win || !xcb_load()) return NULL;
    struct xwin *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    InitializeSRWLock(&x->lk);
    x->win = win;
    uint32_t w, h;
    const char *name = dpy ? xlib_display_name(dpy) : NULL;
    xcb_connection_t *c = g_xcb.connect(name, NULL);
    if (c && !g_xcb.has_error(c) && xw_geometry(c, win, &w, &h, &x->depth)) {
        x->c = c, x->own = 1;
    } else {
        if (c) g_xcb.disconnect(c);
        // 自分の口で見えない：アプリの口で描く（誤りはアプリに届きうる）
        c = appc ? appc : xlib_xcb(dpy);
        if (!c || !xw_geometry(c, win, &w, &h, &x->depth)) {
            say("X の窓 0x%x が見えないので、描けない", win);
            free(x);
            return NULL;
        }
        x->c = c;
        say("自分の X の口でその窓が見えないので、アプリの口で描く");
    }
    uint32_t mr = g_xcb.max_req(x->c); // 4 バイト単位
    x->maxbytes = mr > 64 ? (mr - 8) * 4 : 256 * 1024;
    say("X の窓 0x%x（%u x %u・深さ %u・1 回で送れる %u KB）に描く", win, w, h, x->depth, x->maxbytes / 1024);
    return x;
}

static void xw_close(struct xwin *x) {
    if (!x) return;
    if (x->gc) g_xcb.free_gc(x->c, x->gc);
    if (x->own) g_xcb.disconnect(x->c);
    else g_xcb.flush(x->c);
    free(x->tmp);
    free(x);
}

// ---- surface ----
// 中身は VkIcdSurfaceXcb か VkIcdSurfaceXlib（ローダーが作った物と同じ形）。後ろに描く窓をつける
struct lsurf {
    union { VkIcdSurfaceBase base; VkIcdSurfaceXcb xcb; VkIcdSurfaceXlib xlib; } u;
    struct xwin *w; // 最初に使うときに開く
};
static SRWLOCK g_surf_lock = SRWLOCK_INIT;

static VkResult VKAPI_CALL create_xcb_surface(VkInstance inst, const VkXcbSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    (void) inst; (void) a;
    struct lsurf *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->u.xcb.base.platform = VK_ICD_WSI_PLATFORM_XCB;
    s->u.xcb.connection = ci->connection;
    s->u.xcb.window = ci->window;
    say("窓の口を作った（xcb・窓 0x%x）", (unsigned) ci->window);
    *out = (VkSurfaceKHR) (uintptr_t) s;
    return VK_SUCCESS;
}

static VkResult VKAPI_CALL create_xlib_surface(VkInstance inst, const VkXlibSurfaceCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out) {
    (void) inst; (void) a;
    struct lsurf *s = calloc(1, sizeof *s);
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->u.xlib.base.platform = VK_ICD_WSI_PLATFORM_XLIB;
    s->u.xlib.dpy = ci->dpy;
    s->u.xlib.window = ci->window;
    say("窓の口を作った（Xlib・窓 0x%lx）", (unsigned long) ci->window);
    *out = (VkSurfaceKHR) (uintptr_t) s;
    return VK_SUCCESS;
}

static void VKAPI_CALL destroy_surface(VkInstance inst, VkSurfaceKHR h, const VkAllocationCallbacks *a) {
    (void) inst; (void) a;
    struct lsurf *s = (struct lsurf *) (uintptr_t) h;
    if (!s) return;
    xw_close(s->w);
    free(s);
}

static HWND surface_hwnd(VkSurfaceKHR h) {
    struct lsurf *s = (struct lsurf *) (uintptr_t) h;
    if (!s) return NULL;
    AcquireSRWLockExclusive(&g_surf_lock);
    if (!s->w) {
        if (s->u.base.platform == VK_ICD_WSI_PLATFORM_XCB) s->w = xw_open(s->u.xcb.connection, NULL, s->u.xcb.window);
        else if (s->u.base.platform == VK_ICD_WSI_PLATFORM_XLIB) s->w = xw_open(NULL, s->u.xlib.dpy, (uint32_t) s->u.xlib.window);
    }
    struct xwin *w = s->w;
    ReleaseSRWLockExclusive(&g_surf_lock);
    return (HWND) w;
}

static VkBool32 VKAPI_CALL xcb_presentation_support(VkPhysicalDevice pd, uint32_t qf, xcb_connection_t *c, xcb_visualid_t v) {
    (void) pd; (void) c; (void) v;
    return qf < g_hw.nq && (g_hw.q[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT);
}

static VkBool32 VKAPI_CALL xlib_presentation_support(VkPhysicalDevice pd, uint32_t qf, Display *d, VisualID v) {
    (void) pd; (void) d; (void) v;
    return qf < g_hw.nq && (g_hw.q[qf].queueFlags & VK_QUEUE_GRAPHICS_BIT);
}

// ---- swapchain が使う細い関数 ----
// 窓の中の大きさ。分からなければ 0（窓が消えた・X が切れた）
static int win_size(HWND hw, uint32_t *cw, uint32_t *ch) {
    struct xwin *x = (struct xwin *) hw;
    if (!x || x->dead) return 0;
    AcquireSRWLockExclusive(&x->lk);
    uint8_t depth = 0;
    int ok = xw_geometry(x->c, x->win, cw, ch, &depth);
    if (ok) x->depth = depth;
    ReleaseSRWLockExclusive(&x->lk);
    return ok;
}

// w x h の B・G・R・A の絵（上から下へ）を、窓いっぱいに伸ばして描く
static void win_draw(HWND hw, const uint8_t *px, uint32_t w, uint32_t h) {
    struct xwin *x = (struct xwin *) hw;
    if (!x || x->dead || !px || !w || !h) return;
    AcquireSRWLockExclusive(&x->lk);
    uint32_t ww = 0, wh = 0;
    uint8_t depth = 0;
    if (!xw_geometry(x->c, x->win, &ww, &wh, &depth)) {
        if (!x->warned++) say("X の窓 0x%x が見えなくなった（閉じた）", x->win);
        if (g_xcb.has_error(x->c)) x->dead = 1;
        ReleaseSRWLockExclusive(&x->lk);
        return;
    }
    if (depth != 24 && depth != 32) {
        if (!x->warned++) say("X の窓の深さ %u には描けない（24 か 32 ビットだけ）", depth);
        ReleaseSRWLockExclusive(&x->lk);
        return;
    }
    if (!ww || !wh) { ReleaseSRWLockExclusive(&x->lk); return; }
    if (!x->gc) {
        x->gc = g_xcb.generate_id(x->c);
        g_xcb.create_gc(x->c, x->gc, x->win, 0, NULL);
    }
    const uint8_t *src = px;
    if (ww != w || wh != h) {
        // 窓の大きさに最近傍で伸ばす（StretchDIBits の COLORONCOLOR と同じ）
        size_t need = (size_t) ww * wh * 4;
        if (x->ntmp < need) {
            free(x->tmp);
            x->tmp = malloc(need);
            x->ntmp = x->tmp ? need : 0;
        }
        if (!x->tmp) { ReleaseSRWLockExclusive(&x->lk); return; }
        for (uint32_t y = 0; y < wh; y++) {
            const uint32_t *sr = (const uint32_t *) (px + (size_t) (y * (uint64_t) h / wh) * w * 4);
            uint32_t *dr = (uint32_t *) (x->tmp + (size_t) y * ww * 4);
            for (uint32_t i = 0; i < ww; i++) dr[i] = sr[i * (uint64_t) w / ww];
        }
        src = x->tmp;
    }
    // 1 回で送れる量に、行で分けて送る
    uint32_t stride = ww * 4, rows = x->maxbytes / stride;
    if (!rows) rows = 1;
    for (uint32_t y = 0; y < wh; y += rows) {
        uint32_t n = wh - y < rows ? wh - y : rows;
        g_xcb.put_image(x->c, 2 /* ZPixmap */, x->win, x->gc, (uint16_t) ww, (uint16_t) n, 0, (int16_t) y, 0, depth, n * stride, src + (size_t) y * stride);
    }
    g_xcb.flush(x->c);
    // 自分の口に届いた誤りや知らせは読んで捨てる（ためない）
    if (x->own) {
        void *ev;
        while ((ev = g_xcb.poll_for_event(x->c))) free(ev);
    }
    ReleaseSRWLockExclusive(&x->lk);
}

// 窓をモニターへ動かす（Windows だけ。X11 では窓の置き場所はアプリとウィンドウマネージャーに任せる）
static void move_to_monitor(HWND hwnd) { (void) hwnd; }

// 書き換えの速さ。X11 では聞かずに IPHONE_VK_HZ（標準 60）
static double monitor_hz(HWND w) {
    (void) w;
    const char *e = getenv("IPHONE_VK_HZ");
    return e && atof(e) > 1 ? atof(e) : 60;
}

// ---- JPEG をほどく：libturbojpeg（dlopen）か stb_image ----
// 関数は外に出さない（組むときの -fvisibility=hidden）ので、アプリが持つ別の stb_image と取り違えない
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_ASSERT(x) ((void) 0)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#include "../third_party/stb/stb_image.h"
#pragma GCC diagnostic pop

static struct {
    int tried, ok;
    void *(*init)(void);
    int (*header)(void *, const unsigned char *, unsigned long, int *, int *, int *, int *);
    int (*decomp)(void *, const unsigned char *, unsigned long, unsigned char *, int, int, int, int, int);
    int (*destroy)(void *);
} g_tj;

static int tj_load(void) {
    AcquireSRWLockExclusive(&g_xcb_lock);
    if (!g_tj.tried) {
        g_tj.tried = 1;
        const char *e = getenv("IPHONE_VK_JPEG_DEC");
        void *h = e && !strcmp(e, "stb") ? NULL : dlopen("libturbojpeg.so.0", RTLD_NOW | RTLD_LOCAL);
        if (h) {
            *(void **) &g_tj.init = dlsym(h, "tjInitDecompress");
            *(void **) &g_tj.header = dlsym(h, "tjDecompressHeader3");
            *(void **) &g_tj.decomp = dlsym(h, "tjDecompress2");
            *(void **) &g_tj.destroy = dlsym(h, "tjDestroy");
            g_tj.ok = g_tj.init && g_tj.header && g_tj.decomp && g_tj.destroy;
        }
        say("窓の絵の JPEG は %s でほどく", g_tj.ok ? "libturbojpeg" : "stb_image");
    }
    ReleaseSRWLockExclusive(&g_xcb_lock);
    return g_tj.ok;
}

struct jdec { void *tj; };
static int jdec_open(struct jdec *j) {
    j->tj = tj_load() ? g_tj.init() : NULL;
    return 1; // stb は道具が要らない
}
static void jdec_close(struct jdec *j) {
    if (j->tj) g_tj.destroy(j->tj);
    j->tj = NULL;
}
// JPEG をほどいて、w x h の B・G・R・A の並びにする。1 ほどけた・-1 大きさが違う（窓の大きさが変わる前の絵）・0 失敗
static int jdec_decode(struct jdec *j, const uint8_t *data, size_t n, uint32_t w, uint32_t h, uint8_t *px) {
    if (!n || n > 0x7fffffff) return 0;
    int jw = 0, jh = 0;
    if (j->tj) {
        int ss, cs;
        if (g_tj.header(j->tj, data, (unsigned long) n, &jw, &jh, &ss, &cs) != 0) return 0;
        if ((uint32_t) jw != w || (uint32_t) jh != h) return -1;
        return g_tj.decomp(j->tj, data, (unsigned long) n, px, (int) w, (int) w * 4, (int) h, 8 /* TJPF_BGRA */, 0) == 0;
    }
    int comp;
    if (!stbi_info_from_memory(data, (int) n, &jw, &jh, &comp)) return 0;
    if ((uint32_t) jw != w || (uint32_t) jh != h) return -1;
    stbi_uc *rgba = stbi_load_from_memory(data, (int) n, &jw, &jh, &comp, 4);
    if (!rgba) return 0;
    for (size_t i = 0; i < (size_t) w * h; i++) {
        px[i * 4] = rgba[i * 4 + 2];
        px[i * 4 + 1] = rgba[i * 4 + 1];
        px[i * 4 + 2] = rgba[i * 4];
        px[i * 4 + 3] = 255;
    }
    stbi_image_free(rgba);
    return 1;
}
