// PipeWire（libpipewire-0.3.so.0）を dlopen して、流れ（pw_stream）を 1 本ずつ作る薄い層。組むときに PipeWire のヘッダは要らない。
// webcam_rx（Video/Source：仮想のカメラ）と ipaudio（Audio/Source：仮想のマイク・Audio/Sink：仮想のスピーカー）が使う。
// 形の取り決め（SPA の POD）は、spa/pod/builder.h と同じ並びをここで手で書く。使う番号は下の PWL_ の定め（PipeWire 0.3 から 1.x まで同じ）。
// 構造体（pw_buffer・spa_buffer・spa_data・spa_chunk・pw_stream_events）の並びも PipeWire 0.3 から 1.x まで同じ。
// pw_buffer の requested は 0.3.49 から（それより古ければ読まない）。pw_stream_trigger_process は 0.3.34 から（カメラが使う）。
// PulseAudio（libpulse-simple.so.0）の薄い層も置く（PipeWire が音を受け持っていない PC のため。ipaudio が使う）
#pragma once
#include <dlfcn.h>
#define PWL_FN static __attribute__((unused))
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- SPA の番号（spa/utils/type.h・spa/param/*.h と同じ） ----
#define PWL_TYPE_Id 3u
#define PWL_TYPE_Int 4u
#define PWL_TYPE_Rectangle 10u
#define PWL_TYPE_Fraction 11u
#define PWL_TYPE_Array 13u
#define PWL_TYPE_Object 15u
#define PWL_TYPE_Choice 19u
#define PWL_TYPE_OBJECT_Format 0x40003u
#define PWL_TYPE_OBJECT_ParamBuffers 0x40004u
#define PWL_PARAM_EnumFormat 3u
#define PWL_PARAM_Format 4u
#define PWL_PARAM_Buffers 5u
#define PWL_CHOICE_Range 1u
#define PWL_CHOICE_Enum 3u
#define PWL_CHOICE_Flags 4u
#define PWL_FORMAT_mediaType 1u
#define PWL_FORMAT_mediaSubtype 2u
#define PWL_MEDIA_TYPE_audio 1u
#define PWL_MEDIA_TYPE_video 2u
#define PWL_MEDIA_SUBTYPE_raw 1u
#define PWL_FORMAT_AUDIO_format 0x10001u
#define PWL_FORMAT_AUDIO_rate 0x10003u
#define PWL_FORMAT_AUDIO_channels 0x10004u
#define PWL_FORMAT_AUDIO_position 0x10005u
#define PWL_AUDIO_FORMAT_S16_LE 0x103u
#define PWL_AUDIO_CHANNEL_MONO 2u
#define PWL_AUDIO_CHANNEL_FL 3u
#define PWL_AUDIO_CHANNEL_FR 4u
#define PWL_FORMAT_VIDEO_format 0x20001u
#define PWL_FORMAT_VIDEO_size 0x20003u
#define PWL_FORMAT_VIDEO_framerate 0x20004u
#define PWL_VIDEO_FORMAT_I420 2u
#define PWL_VIDEO_FORMAT_YUY2 4u
#define PWL_VIDEO_FORMAT_NV12 23u
#define PWL_PARAM_BUFFERS_buffers 1u
#define PWL_PARAM_BUFFERS_blocks 2u
#define PWL_PARAM_BUFFERS_size 3u
#define PWL_PARAM_BUFFERS_stride 4u
#define PWL_PARAM_BUFFERS_dataType 6u
#define PWL_DATA_MemPtr 1u
#define PWL_DATA_MemFd 2u
#define PWL_DIRECTION_INPUT 0u
#define PWL_DIRECTION_OUTPUT 1u
#define PWL_ID_ANY 0xffffffffu
#define PWL_STREAM_FLAG_AUTOCONNECT (1u << 0)
#define PWL_STREAM_FLAG_MAP_BUFFERS (1u << 2)
#define PWL_STREAM_FLAG_DRIVER (1u << 3)
#define PWL_STREAM_FLAG_RT_PROCESS (1u << 4)
#define PWL_STREAM_STATE_ERROR (-1)
#define PWL_STREAM_STATE_UNCONNECTED 0
#define PWL_STREAM_STATE_CONNECTING 1
#define PWL_STREAM_STATE_PAUSED 2
#define PWL_STREAM_STATE_STREAMING 3

// ---- 構造体（spa/buffer/buffer.h・pipewire/stream.h と同じ並び） ----
struct pwl_chunk { uint32_t offset, size; int32_t stride, flags; };
struct pwl_data { uint32_t type, flags; int64_t fd; uint32_t mapoffset, maxsize; void *data; struct pwl_chunk *chunk; };
struct pwl_spa_buffer { uint32_t n_metas, n_datas; void *metas; struct pwl_data *datas; };
struct pwl_buffer { struct pwl_spa_buffer *buffer; void *user_data; uint64_t size; uint64_t requested; };
struct pwl_stream_events {
    uint32_t version;
    void (*destroy)(void *data);
    void (*state_changed)(void *data, int old, int state, const char *error);
    void (*control_info)(void *data, uint32_t id, const void *control);
    void (*io_changed)(void *data, uint32_t id, void *area, uint32_t size);
    void (*param_changed)(void *data, uint32_t id, const void *param);
    void (*add_buffer)(void *data, struct pwl_buffer *b);
    void (*remove_buffer)(void *data, struct pwl_buffer *b);
    void (*process)(void *data);
    void (*drained)(void *data);
    void (*command)(void *data, const void *command);
    void (*trigger_done)(void *data);
};

static struct {
    int tried, ok, ver[3];
    void (*init)(int *, char ***);
    const char *(*version)(void);
    void *(*tl_new)(const char *, const void *);
    void *(*tl_loop)(void *);
    int (*tl_start)(void *);
    void (*tl_stop)(void *);
    void (*tl_destroy)(void *);
    void (*tl_lock)(void *);
    void (*tl_unlock)(void *);
    void *(*props_new)(const char *, ...);
    void *(*stream_new_simple)(void *, const char *, void *, const struct pwl_stream_events *, void *);
    int (*stream_connect)(void *, uint32_t, uint32_t, uint32_t, const void **, uint32_t);
    void (*stream_destroy)(void *);
    struct pwl_buffer *(*dequeue)(void *);
    int (*queue)(void *, struct pwl_buffer *);
    int (*update_params)(void *, const void **, uint32_t);
    int (*trigger)(void *); // 0.3.34 から
} g_pw;

// libpipewire-0.3.so.0 を読む。1 読めた
PWL_FN int pwl_load(void) {
    if (g_pw.tried) return g_pw.ok;
    g_pw.tried = 1;
    void *h = dlopen("libpipewire-0.3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 0;
#define PWS(f, n) (*(void **) &g_pw.f = dlsym(h, n))
    int ok = PWS(init, "pw_init") && PWS(version, "pw_get_library_version") && PWS(tl_new, "pw_thread_loop_new") && PWS(tl_loop, "pw_thread_loop_get_loop") &&
             PWS(tl_start, "pw_thread_loop_start") && PWS(tl_stop, "pw_thread_loop_stop") && PWS(tl_destroy, "pw_thread_loop_destroy") &&
             PWS(tl_lock, "pw_thread_loop_lock") && PWS(tl_unlock, "pw_thread_loop_unlock") && PWS(props_new, "pw_properties_new") &&
             PWS(stream_new_simple, "pw_stream_new_simple") && PWS(stream_connect, "pw_stream_connect") && PWS(stream_destroy, "pw_stream_destroy") &&
             PWS(dequeue, "pw_stream_dequeue_buffer") && PWS(queue, "pw_stream_queue_buffer") && PWS(update_params, "pw_stream_update_params");
    PWS(trigger, "pw_stream_trigger_process");
#undef PWS
    if (!ok) return 0;
    g_pw.init(NULL, NULL);
    const char *v = g_pw.version();
    if (v) sscanf(v, "%d.%d.%d", &g_pw.ver[0], &g_pw.ver[1], &g_pw.ver[2]);
    g_pw.ok = 1;
    return 1;
}
PWL_FN int pwl_at_least(int a, int b, int c) {
    return g_pw.ver[0] != a ? g_pw.ver[0] > a : g_pw.ver[1] != b ? g_pw.ver[1] > b : g_pw.ver[2] >= c;
}

// ---- POD を作る（spa_pod_builder と同じ並び。中の子の詰め物は親の大きさに入れる） ----
typedef struct { uint8_t b[1024]; uint32_t n; int bad; } pwl_pod;
PWL_FN void pod_raw(pwl_pod *p, const void *d, uint32_t n) {
    if (p->n + n > sizeof p->b) { p->bad = 1; return; }
    memcpy(p->b + p->n, d, n);
    p->n += n;
}
PWL_FN void pod_u32(pwl_pod *p, uint32_t v) { pod_raw(p, &v, 4); }
PWL_FN void pod_pad(pwl_pod *p) {
    static const uint8_t z[8] = { 0 };
    if (p->n % 8) pod_raw(p, z, 8 - p->n % 8);
}
PWL_FN uint32_t pod_obj_begin(pwl_pod *p, uint32_t type, uint32_t id) {
    uint32_t at = p->n;
    pod_u32(p, 0); // 大きさ（あとで）
    pod_u32(p, PWL_TYPE_Object);
    pod_u32(p, type);
    pod_u32(p, id);
    return at;
}
PWL_FN void pod_obj_end(pwl_pod *p, uint32_t at) {
    uint32_t size = p->n - at - 8;
    memcpy(p->b + at, &size, 4);
}
PWL_FN void pod_key(pwl_pod *p, uint32_t key) { pod_u32(p, key), pod_u32(p, 0); }
PWL_FN void pod_prop_val(pwl_pod *p, uint32_t key, uint32_t type, const void *v, uint32_t n) {
    pod_key(p, key);
    pod_u32(p, n), pod_u32(p, type);
    pod_raw(p, v, n);
    pod_pad(p);
}
PWL_FN void pod_prop_id(pwl_pod *p, uint32_t key, uint32_t v) { pod_prop_val(p, key, PWL_TYPE_Id, &v, 4); }
PWL_FN void pod_prop_int(pwl_pod *p, uint32_t key, int32_t v) { pod_prop_val(p, key, PWL_TYPE_Int, &v, 4); }
PWL_FN void pod_prop_rect(pwl_pod *p, uint32_t key, uint32_t w, uint32_t h) { uint32_t v[2] = { w, h }; pod_prop_val(p, key, PWL_TYPE_Rectangle, v, 8); }
PWL_FN void pod_prop_frac(pwl_pod *p, uint32_t key, uint32_t num, uint32_t den) { uint32_t v[2] = { num, den }; pod_prop_val(p, key, PWL_TYPE_Fraction, v, 8); }
// 選択肢（choice）：値は 4 バイトずつ（Id か Int）。Enum は先頭が既定・そのあとが選べる値。Range は既定・最小・最大。Flags は 1 つ
PWL_FN void pod_prop_choice(pwl_pod *p, uint32_t key, uint32_t ctype, uint32_t vtype, const uint32_t *v, uint32_t n) {
    pod_key(p, key);
    pod_u32(p, 16 + 4 * n), pod_u32(p, PWL_TYPE_Choice);
    pod_u32(p, ctype), pod_u32(p, 0);
    pod_u32(p, 4), pod_u32(p, vtype);
    pod_raw(p, v, 4 * n);
    pod_pad(p);
}
PWL_FN void pod_prop_array_id(pwl_pod *p, uint32_t key, const uint32_t *v, uint32_t n) {
    pod_key(p, key);
    pod_u32(p, 8 + 4 * n), pod_u32(p, PWL_TYPE_Array);
    pod_u32(p, 4), pod_u32(p, PWL_TYPE_Id);
    pod_raw(p, v, 4 * n);
    pod_pad(p);
}

// ---- POD を読む：Format の object から key の値（Id・Int なら 1 つ・Rectangle なら 2 つ）。choice なら既定の値。1 読めた ----
PWL_FN int pod_find(const void *param, uint32_t key, uint32_t *out, int nout) {
    const uint8_t *p = (const uint8_t *) param;
    uint32_t size, type;
    memcpy(&size, p, 4), memcpy(&type, p + 4, 4);
    if (type != PWL_TYPE_Object || size < 8 || size > (1u << 20)) return 0;
    uint32_t at = 16, end = 8 + size;
    while (at + 16 <= end) {
        uint32_t k, vs, vt;
        memcpy(&k, p + at, 4), memcpy(&vs, p + at + 8, 4), memcpy(&vt, p + at + 12, 4);
        const uint8_t *v = p + at + 16;
        if (at + 16 + vs > end) return 0;
        if (k == key) {
            if (vt == PWL_TYPE_Choice) {
                if (vs < 16) return 0;
                uint32_t cs, ct;
                memcpy(&cs, v + 8, 4), memcpy(&ct, v + 12, 4);
                if (cs < 4u * (uint32_t) nout || vs < 16 + cs) return 0;
                vt = ct, v += 16, vs = cs;
            }
            if (vs < 4u * (uint32_t) nout) return 0;
            memcpy(out, v, 4u * (uint32_t) nout);
            return 1;
        }
        at += 16 + ((vs + 7) & ~7u);
    }
    return 0;
}

// ---- PulseAudio（libpulse-simple.so.0）。PipeWire が音を受け持っていない PC のため ----
typedef struct { int format; uint32_t rate; uint8_t channels; } pwl_pa_spec;           // pa_sample_spec
typedef struct { uint32_t maxlength, tlength, prebuf, minreq, fragsize; } pwl_pa_attr; // pa_buffer_attr
#define PWL_PA_SAMPLE_S16LE 3
#define PWL_PA_STREAM_PLAYBACK 1
#define PWL_PA_STREAM_RECORD 2
static struct {
    int tried, ok;
    void *(*open)(const char *, const char *, int, const char *, const char *, const pwl_pa_spec *, const void *, const pwl_pa_attr *, int *);
    void (*close)(void *);
    int (*write)(void *, const void *, size_t, int *);
    int (*read)(void *, void *, size_t, int *);
    const char *(*err)(int);
} g_pa;
PWL_FN int pa_load(void) {
    if (g_pa.tried) return g_pa.ok;
    g_pa.tried = 1;
    void *h = dlopen("libpulse-simple.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!h) return 0;
    *(void **) &g_pa.open = dlsym(h, "pa_simple_new");
    *(void **) &g_pa.close = dlsym(h, "pa_simple_free");
    *(void **) &g_pa.write = dlsym(h, "pa_simple_write");
    *(void **) &g_pa.read = dlsym(h, "pa_simple_read");
    *(void **) &g_pa.err = dlsym(h, "pa_strerror"); // libpulse.so.0（libpulse-simple が読み込む）
    g_pa.ok = g_pa.open && g_pa.close && g_pa.write && g_pa.read;
    return g_pa.ok;
}
PWL_FN const char *pa_errtext(int e) { return g_pa.err ? g_pa.err(e) : "?"; }
