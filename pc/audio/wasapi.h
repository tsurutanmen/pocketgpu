// WASAPI の道具。機器を名前で選ぶ・一覧を出す・loopback で取る・鳴らす。ipaudio.c だけが読む
#pragma once
#define COBJMACROS
#include "proto.h"
#include <initguid.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <ctype.h>

#define WA_CHECK(call, what) do { HRESULT _h = (call); if (FAILED(_h)) { fprintf(stderr, "失敗 %s: 0x%08lx\n", what, (unsigned long) _h); goto fail; } } while (0)

static IMMDeviceEnumerator *g_enum;

static int wa_init(void) {
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 0;
    return SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **) &g_enum));
}

static void wa_utf8(const wchar_t *w, char *out, int n) {
    int r = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, n, NULL, NULL);
    if (r <= 0) out[0] = 0;
}

static void wa_name(IMMDevice *d, char *out, int n) {
    IPropertyStore *ps = NULL;
    PROPVARIANT v;
    PropVariantInit(&v);
    out[0] = 0;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) {
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) wa_utf8(v.pwszVal, out, n);
        PropVariantClear(&v);
        IPropertyStore_Release(ps);
    }
}

static int wa_icontains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char) hay[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

// 名前に needle を含む、動いている機器の最初の 1 つ。needle が NULL か "default" なら既定の機器
static IMMDevice *wa_find(EDataFlow flow, const char *needle) {
    IMMDevice *d = NULL;
    if (!needle || !*needle || !strcmp(needle, "default")) {
        if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_enum, flow, eConsole, &d))) return NULL;
        return d;
    }
    IMMDeviceCollection *col = NULL;
    if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(g_enum, flow, DEVICE_STATE_ACTIVE, &col))) return NULL;
    UINT n = 0;
    IMMDeviceCollection_GetCount(col, &n);
    for (UINT i = 0; i < n && !d; i++) {
        IMMDevice *c = NULL;
        char nm[256];
        if (FAILED(IMMDeviceCollection_Item(col, i, &c))) continue;
        wa_name(c, nm, sizeof nm);
        if (wa_icontains(nm, needle)) d = c; else IMMDevice_Release(c);
    }
    IMMDeviceCollection_Release(col);
    return d;
}

static int wa_is_float(const WAVEFORMATEX *wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return 1;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const WAVEFORMATEXTENSIBLE *e = (const WAVEFORMATEXTENSIBLE *) wf;
        return e->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    return 0;
}

static DWORD wa_chmask(const WAVEFORMATEX *wf) {
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) return ((const WAVEFORMATEXTENSIBLE *) wf)->dwChannelMask;
    return wf->nChannels == 1 ? 0x4 : 0x3;
}

static float wa_volume(IMMDevice *d, int *mute) {
    IAudioEndpointVolume *v = NULL;
    float x = -1;
    BOOL m = FALSE;
    if (SUCCEEDED(IMMDevice_Activate(d, &IID_IAudioEndpointVolume, CLSCTX_ALL, NULL, (void **) &v))) {
        IAudioEndpointVolume_GetMasterVolumeLevelScalar(v, &x);
        IAudioEndpointVolume_GetMute(v, &m);
        IAudioEndpointVolume_Release(v);
    }
    if (mute) *mute = m;
    return x;
}

static void wa_list(void) {
    static const struct { EDataFlow f; const char *t; } fl[2] = { { eRender, "出力" }, { eCapture, "入力" } };
    for (int k = 0; k < 2; k++) {
        IMMDevice *def = NULL;
        wchar_t defid[256] = L"";
        if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_enum, fl[k].f, eConsole, &def))) {
            LPWSTR id = NULL;
            if (SUCCEEDED(IMMDevice_GetId(def, &id))) { wcsncpy(defid, id, 255); CoTaskMemFree(id); }
            IMMDevice_Release(def);
        }
        IMMDeviceCollection *col = NULL;
        if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(g_enum, fl[k].f, DEVICE_STATE_ACTIVE | DEVICE_STATE_DISABLED | DEVICE_STATE_UNPLUGGED, &col))) continue;
        UINT n = 0;
        IMMDeviceCollection_GetCount(col, &n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice *d = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
            char nm[256];
            wa_name(d, nm, sizeof nm);
            DWORD st = 0;
            IMMDevice_GetState(d, &st);
            LPWSTR id = NULL;
            IMMDevice_GetId(d, &id);
            int isdef = id && !wcscmp(id, defid);
            char info[128] = "";
            if (st == DEVICE_STATE_ACTIVE) {
                IAudioClient *ac = NULL;
                if (SUCCEEDED(IMMDevice_Activate(d, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **) &ac))) {
                    WAVEFORMATEX *wf = NULL;
                    if (SUCCEEDED(IAudioClient_GetMixFormat(ac, &wf))) {
                        int mute = 0;
                        float vol = wa_volume(d, &mute);
                        snprintf(info, sizeof info, "%luHz %uch %s 音量%.2f%s", (unsigned long) wf->nSamplesPerSec, wf->nChannels,
                                 wa_is_float(wf) ? "float" : "pcm", vol, mute ? " ミュート" : "");
                        CoTaskMemFree(wf);
                    }
                    IAudioClient_Release(ac);
                }
            }
            printf("%s%s [%s] %s  %s\n", fl[k].t, isdef ? "(既定)" : "", st == DEVICE_STATE_ACTIVE ? "動作中" : st == DEVICE_STATE_DISABLED ? "無効" : st == DEVICE_STATE_UNPLUGGED ? "抜けている" : "?", nm, info);
            if (id) CoTaskMemFree(id);
            IMMDevice_Release(d);
        }
        IMMDeviceCollection_Release(col);
    }
}

// ---- 取る（loopback または普通の入力）----
typedef struct {
    IMMDevice *dev;
    IAudioClient *ac;
    IAudioCaptureClient *cap;
    HANDLE ev;
    WAVEFORMATEX *wf;
    int rate, ch, is_float, bits, conv;
    DWORD mask;
    char name[256];
} WaCap;

// want_rate が 0 でなければ、その rate の 16 ビット・2 チャンネルで取るよう頼む（OS が変換する）。だめなら機器の混ぜ形式で取る
// flow は loopback のとき eRender、普通の入力のとき eCapture の機器を渡す
static int wa_cap_open(WaCap *c, IMMDevice *d, int loopback, int want_rate) {
    memset(c, 0, sizeof *c);
    c->dev = d;
    c->conv = 0;
    wa_name(d, c->name, sizeof c->name);
    DWORD flags = (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0) | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    c->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (want_rate) {
        WAVEFORMATEX *own = (WAVEFORMATEX *) CoTaskMemAlloc(sizeof(WAVEFORMATEX));
        memset(own, 0, sizeof *own);
        own->wFormatTag = WAVE_FORMAT_PCM;
        own->nChannels = 2;
        own->nSamplesPerSec = (DWORD) want_rate;
        own->wBitsPerSample = 16;
        own->nBlockAlign = 4;
        own->nAvgBytesPerSec = (DWORD) want_rate * 4;
        if (SUCCEEDED(IMMDevice_Activate(d, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **) &c->ac)) &&
            SUCCEEDED(IAudioClient_Initialize(c->ac, AUDCLNT_SHAREMODE_SHARED, flags | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                              5000000, 0, own, NULL))) {
            c->wf = own;
            c->conv = 1;
        } else {
            CoTaskMemFree(own);
            if (c->ac) { IAudioClient_Release(c->ac); c->ac = NULL; }
        }
    }
    if (!c->ac) {
        WA_CHECK(IMMDevice_Activate(d, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **) &c->ac), "Activate");
        WA_CHECK(IAudioClient_GetMixFormat(c->ac, &c->wf), "GetMixFormat");
        if (!(wa_is_float(c->wf) && c->wf->wBitsPerSample == 32) && !(!wa_is_float(c->wf) && c->wf->wBitsPerSample == 16)) {
            fprintf(stderr, "対応していない形式: %d bit %s\n", c->wf->wBitsPerSample, wa_is_float(c->wf) ? "float" : "pcm");
            return 0;
        }
        WA_CHECK(IAudioClient_Initialize(c->ac, AUDCLNT_SHAREMODE_SHARED, flags, 5000000, 0, c->wf, NULL), "Initialize(capture)");
    }
    c->rate = (int) c->wf->nSamplesPerSec;
    c->ch = c->wf->nChannels;
    c->bits = c->wf->wBitsPerSample;
    c->is_float = wa_is_float(c->wf);
    c->mask = wa_chmask(c->wf);
    WA_CHECK(IAudioClient_SetEventHandle(c->ac, c->ev), "SetEventHandle");
    WA_CHECK(IAudioClient_GetService(c->ac, &IID_IAudioCaptureClient, (void **) &c->cap), "GetService(capture)");
    return 1;
fail:
    return 0;
}

// 1 コマ分を 16 ビットの 2 チャンネルにする（surround は前の 2 つに畳む）
static inline int16_t wa_clip16(float v) {
    v *= 32768.0f;
    return (int16_t) (v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
}

static void wa_to_stereo_s16(const WaCap *c, const void *in, int frames, int16_t *out) {
    int ch = c->ch;
    for (int i = 0; i < frames; i++) {
        float l = 0, r = 0;
        float s[16];
        for (int k = 0; k < ch && k < 16; k++) {
            if (c->is_float) s[k] = ((const float *) in)[(size_t) i * ch + k];
            else s[k] = ((const int16_t *) in)[(size_t) i * ch + k] * (1.0f / 32768.0f);
        }
        if (ch == 1) { l = r = s[0]; }
        else if (ch == 2) { l = s[0]; r = s[1]; }
        else {
            // マスクの低い順に、各チャンネルの位置が決まる
            DWORD m = c->mask;
            int k = 0;
            for (int bit = 0; bit < 18 && k < ch && k < 16; bit++) {
                if (!(m & (1u << bit))) continue;
                float v = s[k++];
                switch (1u << bit) {
                    case 0x1: l += v; break;               // FL
                    case 0x2: r += v; break;               // FR
                    case 0x4: l += v * 0.707f; r += v * 0.707f; break; // FC
                    case 0x8: break;                       // LFE は捨てる
                    case 0x10: case 0x200: case 0x40: l += v * 0.5f; break; // BL・SL・FLC は左
                    case 0x20: case 0x400: case 0x80: r += v * 0.5f; break; // BR・SR・FRC は右
                    case 0x100: l += v * 0.35f; r += v * 0.35f; break;      // BC
                    default: break;
                }
            }
        }
        out[2 * i] = wa_clip16(l);
        out[2 * i + 1] = wa_clip16(r);
    }
}

// ---- 鳴らす（共有・イベント駆動・機器の混ぜ形式）----
typedef void (*WaFill)(void *user, float *out, int frames, int ch);

typedef struct {
    IMMDevice *dev;
    IAudioClient *ac;
    IAudioRenderClient *rc;
    HANDLE ev, th;
    WAVEFORMATEX *wf;
    UINT32 buf_frames;
    int rate, ch;
    volatile LONG stop;
    WaFill fill;      // NULL なら無音（0 を書く）
    void *user;
    char name[256];
    uint64_t callbacks;
} WaRender;

static DWORD WINAPI wa_render_thread(LPVOID arg) {
    WaRender *r = (WaRender *) arg;
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    while (!r->stop) {
        if (WaitForSingleObject(r->ev, 200) != WAIT_OBJECT_0) continue;
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(r->ac, &pad))) break;
        UINT32 n = r->buf_frames - pad;
        if (!n) continue;
        BYTE *p = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(r->rc, n, &p))) break;
        if (r->fill) r->fill(r->user, (float *) p, (int) n, r->ch);
        else memset(p, 0, (size_t) n * r->ch * sizeof(float));
        IAudioRenderClient_ReleaseBuffer(r->rc, n, 0);
        r->callbacks++;
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    return 0;
}

// buf_ms：この流れにためる長さ。イベントのたびに満たすので、ほぼそのまま遅れになる（mic2 で足した。前は 100 ms 決め打ち）
static int wa_render_open_ms(WaRender *r, IMMDevice *d, WaFill fill, void *user, int buf_ms) {
    memset(r, 0, sizeof *r);
    r->dev = d;
    r->fill = fill;
    r->user = user;
    wa_name(d, r->name, sizeof r->name);
    WA_CHECK(IMMDevice_Activate(d, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **) &r->ac), "Activate");
    WA_CHECK(IAudioClient_GetMixFormat(r->ac, &r->wf), "GetMixFormat");
    if (!wa_is_float(r->wf) || r->wf->wBitsPerSample != 32) { fprintf(stderr, "出力の混ぜ形式が float32 ではない\n"); return 0; }
    r->rate = (int) r->wf->nSamplesPerSec;
    r->ch = r->wf->nChannels;
    r->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    WA_CHECK(IAudioClient_Initialize(r->ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, (REFERENCE_TIME) buf_ms * 10000, 0, r->wf, NULL), "Initialize(render)");
    WA_CHECK(IAudioClient_SetEventHandle(r->ac, r->ev), "SetEventHandle");
    WA_CHECK(IAudioClient_GetBufferSize(r->ac, &r->buf_frames), "GetBufferSize");
    WA_CHECK(IAudioClient_GetService(r->ac, &IID_IAudioRenderClient, (void **) &r->rc), "GetService(render)");
    return 1;
fail:
    return 0;
}

static int wa_render_open(WaRender *r, IMMDevice *d, WaFill fill, void *user) { return wa_render_open_ms(r, d, fill, user, 100); }

static int wa_render_start(WaRender *r) {
    // 最初に全部埋めてから始める
    BYTE *p = NULL;
    if (FAILED(IAudioRenderClient_GetBuffer(r->rc, r->buf_frames, &p))) return 0;
    if (r->fill) r->fill(r->user, (float *) p, (int) r->buf_frames, r->ch);
    else memset(p, 0, (size_t) r->buf_frames * r->ch * sizeof(float));
    IAudioRenderClient_ReleaseBuffer(r->rc, r->buf_frames, 0);
    if (FAILED(IAudioClient_Start(r->ac))) return 0;
    r->th = CreateThread(NULL, 0, wa_render_thread, r, 0, NULL);
    return r->th != NULL;
}

static void wa_render_stop(WaRender *r) {
    r->stop = 1;
    if (r->th) { WaitForSingleObject(r->th, 2000); CloseHandle(r->th); r->th = NULL; }
    if (r->ac) IAudioClient_Stop(r->ac);
}
