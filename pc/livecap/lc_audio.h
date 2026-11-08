// 音を取る：WASAPI の loopback・マイク・WAV（試験用）。どれも 16 kHz・モノラル・s16 にして on_chunk に渡す。
// work/audio/pc/wasapi.h（音の係）と同じ選び方：名前の一部で選ぶ・無ければ既定の機器。
// loopback は、どのアプリも鳴らしていない間は WASAPI が何も渡してこないので、同じ出力機器に無音を流し続ける（音は出ない）。
// それでも届かない間は、時間の流れを切らさないように 0 で埋める（Python 版の audio_capture.py と同じ）。
// on_chunk の t_end は、その塊の最後のコマを PC が取った時刻（QueryPerformanceCounter の秒）。
#pragma once

#define OUT_RATE 16000

typedef void (*OnChunk)(const int16_t *pcm, int n, double t_end);

enum { SRC_LOOPBACK = 1, SRC_MIC, SRC_WAV };

static IMMDeviceEnumerator *g_enum;

static int wa_init(void) {
    HRESULT h = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(h) && h != RPC_E_CHANGED_MODE) return 0;
    return SUCCEEDED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **) &g_enum));
}

static void wa_name(IMMDevice *d, char *out, int n) {
    IPropertyStore *ps = NULL;
    PROPVARIANT v;
    PropVariantInit(&v);
    out[0] = 0;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(d, STGM_READ, &ps))) {
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR) {
            if (WideCharToMultiByte(CP_UTF8, 0, v.pwszVal, -1, out, n, NULL, NULL) <= 0) out[0] = 0;
        }
        PropVariantClear(&v);
        IPropertyStore_Release(ps);
    }
}

// 大文字と小文字を区別せずに含むか（英字だけ。日本語の名前はそのまま比べる）
static int wa_icontains(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < n && hay[i] && tolower((unsigned char) hay[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == n) return 1;
    }
    return 0;
}

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
        char nm[512];
        if (FAILED(IMMDeviceCollection_Item(col, i, &c))) continue;
        wa_name(c, nm, sizeof nm);
        if (wa_icontains(nm, needle)) d = c;
        else IMMDevice_Release(c);
    }
    IMMDeviceCollection_Release(col);
    return d;
}

static int wa_is_float(const WAVEFORMATEX *wf) {
    if (wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) return 1;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) return ((const WAVEFORMATEXTENSIBLE *) wf)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    return 0;
}
static DWORD wa_chmask(const WAVEFORMATEX *wf) {
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) return ((const WAVEFORMATEXTENSIBLE *) wf)->dwChannelMask;
    return wf->nChannels == 1 ? 0x4 : 0x3;
}

static void wa_list(void) {
    static const EDataFlow fl[2] = { eRender, eCapture };
    static const char *kind[2] = { "loopback", "mic" };
    int any = 0;
    for (int k = 0; k < 2; k++) {
        IMMDevice *def = NULL;
        LPWSTR defid = NULL;
        if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(g_enum, fl[k], eConsole, &def))) {
            IMMDevice_GetId(def, &defid);
            IMMDevice_Release(def);
        }
        IMMDeviceCollection *col = NULL;
        if (FAILED(IMMDeviceEnumerator_EnumAudioEndpoints(g_enum, fl[k], DEVICE_STATE_ACTIVE, &col))) continue;
        UINT n = 0;
        IMMDeviceCollection_GetCount(col, &n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice *d = NULL;
            if (FAILED(IMMDeviceCollection_Item(col, i, &d))) continue;
            char nm[512];
            wa_name(d, nm, sizeof nm);
            LPWSTR id = NULL;
            IMMDevice_GetId(d, &id);
            int isdef = id && defid && !wcscmp(id, defid);
            int rate = 0, ch = 0;
            IAudioClient *ac = NULL;
            if (SUCCEEDED(IMMDevice_Activate(d, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **) &ac))) {
                WAVEFORMATEX *wf = NULL;
                if (SUCCEEDED(IAudioClient_GetMixFormat(ac, &wf))) {
                    rate = (int) wf->nSamplesPerSec;
                    ch = wf->nChannels;
                    CoTaskMemFree(wf);
                }
                IAudioClient_Release(ac);
            }
            say("%-8s %s  %d Hz %dch%s%s", kind[k], nm, rate, ch, isdef ? "  " : "", isdef ? TT(T_DEFAULT) : "");
            any = 1;
            if (id) CoTaskMemFree(id);
            IMMDevice_Release(d);
        }
        IMMDeviceCollection_Release(col);
        if (defid) CoTaskMemFree(defid);
    }
    if (!any) say("%s", TT(T_NO_DEVICES));
}

// ---- 無音を鳴らし続ける（loopback の塊を途切れさせない）----
typedef struct {
    IAudioClient *ac;
    IAudioRenderClient *rc;
    HANDLE ev, th;
    WAVEFORMATEX *wf;
    UINT32 buf_frames;
    volatile LONG stop;
} KeepAlive;

static DWORD WINAPI ka_thread(LPVOID arg) {
    KeepAlive *k = arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    const UINT32 bpf = k->wf->nBlockAlign;
    while (!k->stop) {
        if (WaitForSingleObject(k->ev, 200) != WAIT_OBJECT_0) continue;
        UINT32 pad = 0;
        if (FAILED(IAudioClient_GetCurrentPadding(k->ac, &pad))) break;
        UINT32 n = k->buf_frames - pad;
        if (!n) continue;
        BYTE *p = NULL;
        if (FAILED(IAudioRenderClient_GetBuffer(k->rc, n, &p))) break;
        memset(p, 0, (size_t) n * bpf);
        IAudioRenderClient_ReleaseBuffer(k->rc, n, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    CoUninitialize();
    return 0;
}

static int ka_start(KeepAlive *k, IMMDevice *d) {
    memset(k, 0, sizeof *k);
    if (FAILED(IMMDevice_Activate(d, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **) &k->ac))) return 0;
    if (FAILED(IAudioClient_GetMixFormat(k->ac, &k->wf))) return 0;
    k->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (FAILED(IAudioClient_Initialize(k->ac, AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 1000000, 0, k->wf, NULL))) return 0;
    if (FAILED(IAudioClient_SetEventHandle(k->ac, k->ev))) return 0;
    if (FAILED(IAudioClient_GetBufferSize(k->ac, &k->buf_frames))) return 0;
    if (FAILED(IAudioClient_GetService(k->ac, &IID_IAudioRenderClient, (void **) &k->rc))) return 0;
    BYTE *p = NULL;
    if (FAILED(IAudioRenderClient_GetBuffer(k->rc, k->buf_frames, &p))) return 0;
    memset(p, 0, (size_t) k->buf_frames * k->wf->nBlockAlign);
    IAudioRenderClient_ReleaseBuffer(k->rc, k->buf_frames, AUDCLNT_BUFFERFLAGS_SILENT);
    if (FAILED(IAudioClient_Start(k->ac))) return 0;
    k->th = CreateThread(NULL, 0, ka_thread, k, 0, NULL);
    return k->th != NULL;
}

static void ka_stop(KeepAlive *k) {
    k->stop = 1;
    if (k->th) { WaitForSingleObject(k->th, 2000); CloseHandle(k->th); k->th = NULL; }
    if (k->ac) { IAudioClient_Stop(k->ac); }
}

// ---- 取り込み本体 ----
typedef struct {
    int kind;
    char name[600];
    int rate, ch;
    OnChunk on_chunk;
    volatile LONG stop, done, stopped;
    HANDLE th;
    double produced_until;  // 最後に渡したコマを取った時刻
    int have_pu;
    double filled_s;        // 0 で埋めた秒数
    long long frames_out;
    Rs rs;
    float *mono, *rsout;
    int16_t *s16;
    int mono_cap, out_cap;
    // WASAPI
    IMMDevice *dev;
    IAudioClient *ac;
    IAudioCaptureClient *cap;
    HANDLE ev;
    WAVEFORMATEX *wf;
    int is_float, bits;
    DWORD mask;
    KeepAlive ka;
    int have_ka, keepalive;
    double latency_ms;
    // WAV
    float *wav;
    long long wav_n;
} Source;

static void src_emit16(Source *s, const int16_t *pcm, int n, double t_end) {
    if (n <= 0 || !s->on_chunk) return;
    s->frames_out += n;
    s->produced_until = t_end;
    s->have_pu = 1;
    s->on_chunk(pcm, n, t_end);
}

static void src_feed_zeros(Source *s, long long n, double t_end) {
    memset(s->mono, 0, sizeof(float) * (size_t) s->mono_cap);
    while (n > 0) {
        int m = n > s->mono_cap ? s->mono_cap : (int) n;
        int k = rs_run(&s->rs, s->mono, m, s->rsout);
        if (k > 0) {
            f32_to_s16(s->rsout, s->s16, k);
            src_emit16(s, s->s16, k, t_end);
        }
        n -= m;
    }
}

static inline float wa_sample(const BYTE *p, int bits, int is_float) {
    if (is_float) return bits == 64 ? (float) *(const double *) p : *(const float *) p;
    if (bits == 16) return (float) *(const int16_t *) p * (1.0f / 32768.0f);
    if (bits == 24) return (float) (((int32_t) ((uint32_t) p[0] << 8 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 24)) >> 8) * (1.0f / 8388608.0f);
    if (bits == 32) return (float) *(const int32_t *) p * (1.0f / 2147483648.0f);
    if (bits == 8) return ((float) p[0] - 128.0f) * (1.0f / 128.0f);
    return 0.0f;
}

// n コマを モノラルの float に。2 チャンネルは平均。5.1 などは前の左右に畳んでから平均（中央の声が小さくならない）
static void wa_to_mono(const Source *s, const BYTE *in, int n, float *out) {
    const int ch = s->ch, bps = s->bits / 8, bpf = s->wf->nBlockAlign;
    for (int i = 0; i < n; i++) {
        const BYTE *f = in + (size_t) i * bpf;
        if (ch == 1) { out[i] = wa_sample(f, s->bits, s->is_float); continue; }
        if (ch == 2) { out[i] = 0.5f * (wa_sample(f, s->bits, s->is_float) + wa_sample(f + bps, s->bits, s->is_float)); continue; }
        float l = 0, r = 0;
        DWORD m = s->mask;
        int k = 0;
        for (int bit = 0; bit < 18 && k < ch; bit++) {
            if (!(m & (1u << bit))) continue;
            float v = wa_sample(f + (size_t) k * bps, s->bits, s->is_float);
            k++;
            switch (1u << bit) {
            case 0x1: l += v; break;
            case 0x2: r += v; break;
            case 0x4: l += v * 0.707f; r += v * 0.707f; break;
            case 0x8: break;  // LFE は捨てる
            case 0x10: case 0x200: case 0x40: l += v * 0.5f; break;
            case 0x20: case 0x400: case 0x80: r += v * 0.5f; break;
            case 0x100: l += v * 0.35f; r += v * 0.35f; break;
            default: break;
            }
        }
        if (!m) {  // マスクが無いときは全部の平均
            for (int c = 0; c < ch; c++) l += wa_sample(f + (size_t) c * bps, s->bits, s->is_float);
            out[i] = l / ch;
            continue;
        }
        out[i] = 0.5f * (l + r);
    }
}

static int src_alloc(Source *s) {
    if (!rs_init(&s->rs, s->rate, OUT_RATE)) return 0;
    s->mono_cap = s->rate > 8000 ? s->rate / 2 : 4000;  // 0.5 秒ぶん
    s->out_cap = rs_out_max(&s->rs, s->mono_cap) + 16;
    s->mono = malloc(sizeof(float) * (size_t) s->mono_cap);
    s->rsout = malloc(sizeof(float) * (size_t) s->out_cap);
    s->s16 = malloc(sizeof(int16_t) * (size_t) s->out_cap);
    return s->mono && s->rsout && s->s16;
}

static DWORD WINAPI src_wasapi_thread(LPVOID arg) {
    Source *s = arg;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE mm = AvSetMmThreadCharacteristicsW(L"Audio", &task);
    UINT64 exp = 0;
    int have_dpos = 0, idle = 0;
    const double rate = s->rate;
    while (!s->stop) {
        if (WaitForSingleObject(s->ev, 100) != WAIT_OBJECT_0) {
            // 何も届かない：0.3 秒を超えたら、0.2 秒遅れまで無音で埋める（届き始めた音と重ならないように少し遅らせる）
            double now = now_s();
            if (now - s->produced_until >= 0.3) {
                long long n = (long long) ((now - 0.2 - s->produced_until) * rate);
                if (n > 0) {
                    double t = s->produced_until + n / rate;
                    s->filled_s += n / rate;
                    src_feed_zeros(s, n, t);
                    s->produced_until = t;
                    idle = 1;
                }
            }
            continue;
        }
        for (;;) {
            UINT32 np = 0;
            if (FAILED(IAudioCaptureClient_GetNextPacketSize(s->cap, &np)) || !np) break;
            BYTE *data = NULL;
            UINT32 fr = 0;
            DWORD flags = 0;
            UINT64 dpos = 0, qpc = 0;
            if (FAILED(IAudioCaptureClient_GetBuffer(s->cap, &data, &fr, &flags, &dpos, &qpc))) break;
            double t = now_s();
            if (idle) {  // 埋めていた間のあと：時計で隙間を見る
                double gap = (t - fr / rate) - s->produced_until;
                if (gap > 0.05) {
                    long long n = (long long) (gap * rate);
                    s->filled_s += n / rate;
                    src_feed_zeros(s, n, t - fr / rate);
                }
                idle = 0;
            } else if (have_dpos && dpos > exp) {  // OS の側で取りこぼした：機器の位置が飛んだぶんを無音で
                UINT64 lost = dpos - exp;
                if (lost > (UINT64) s->rate * 2) lost = (UINT64) s->rate * 2;
                s->filled_s += (double) lost / rate;
                src_feed_zeros(s, (long long) lost, t - fr / rate);
            }
            exp = dpos + fr;
            have_dpos = 1;
            UINT32 done = 0;
            while (done < fr) {
                int m = (int) (fr - done) > s->mono_cap ? s->mono_cap : (int) (fr - done);
                if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) memset(s->mono, 0, sizeof(float) * (size_t) m);
                else wa_to_mono(s, data + (size_t) done * s->wf->nBlockAlign, m, s->mono);
                int k = rs_run(&s->rs, s->mono, m, s->rsout);
                if (k > 0) {
                    f32_to_s16(s->rsout, s->s16, k);
                    src_emit16(s, s->s16, k, t);
                }
                done += (UINT32) m;
            }
            IAudioCaptureClient_ReleaseBuffer(s->cap, fr);
        }
    }
    if (mm) AvRevertMmThreadCharacteristics(mm);
    CoUninitialize();
    return 0;
}

// ---- WAV（試験用）：実時間で 20 ms ずつ、取ったばかりの音として渡す ----
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

static void wait_until(HANDLE timer, double due) {
    double w = due - now_s();
    if (w <= 0) return;
    if (timer) {
        LARGE_INTEGER li;
        li.QuadPart = -(LONGLONG) (w * 1e7);
        if (SetWaitableTimer(timer, &li, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(timer, INFINITE);
            return;
        }
    }
    Sleep((DWORD) (w * 1000));
}

static DWORD WINAPI src_wav_thread(LPVOID arg) {
    Source *s = arg;
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(NULL, TRUE, NULL);
    const int chunk = s->rate * 20 / 1000;
    const double t0 = now_s();
    long long i = 0, in_total = 0;
    while (!s->stop && i < s->wav_n) {
        int seg = (int) (s->wav_n - i < chunk ? s->wav_n - i : chunk);
        in_total += seg;
        wait_until(timer, t0 + (double) in_total / s->rate);
        int k = rs_run(&s->rs, s->wav + i, seg, s->rsout);
        i += seg;
        double t = now_s();
        // Python 版は、この糸が 50 ms より遅れると無音を差し込んでいた。WAV は音を落とさないので差し込まない
        // （差し込むと、元に無い間が文の途中に入る。遅れた分は、あとの塊がまとめて追いつく）
        if (k > 0) {
            f32_to_s16(s->rsout, s->s16, k);
            src_emit16(s, s->s16, k, t);
        }
    }
    if (timer) CloseHandle(timer);
    InterlockedExchange(&s->done, 1);
    SetEvent(g_q_event);  // 本体の糸を起こして終わりを見せる
    return 0;
}

// WAV を読んでモノラルの float に。16・24・32 ビットの整数と 32 ビットの float
static int wav_load(Source *s, const char *path) {
    size_t len = 0;
    unsigned char *d = (unsigned char *) read_file_u8(path, &len);
    if (!d) { warn(TT(T_WAV_FAIL), path); return 0; }
    if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) { warn(TT(T_WAV_FORMAT), path); free(d); return 0; }
    int tag = 0, ch = 0, rate = 0, bits = 0;
    const unsigned char *data = NULL;
    size_t dlen = 0, pos = 12;
    while (pos + 8 <= len) {
        uint32_t n;
        memcpy(&n, d + pos + 4, 4);
        const unsigned char *b = d + pos + 8;
        if (!memcmp(d + pos, "fmt ", 4) && n >= 16 && pos + 8 + n <= len) {
            uint16_t t16, c16, b16;
            uint32_t r32;
            memcpy(&t16, b, 2); memcpy(&c16, b + 2, 2); memcpy(&r32, b + 4, 4); memcpy(&b16, b + 14, 2);
            tag = t16; ch = c16; rate = (int) r32; bits = b16;
            if (tag == 0xFFFE && n >= 26) { uint16_t sub; memcpy(&sub, b + 24, 2); tag = sub; }
        } else if (!memcmp(d + pos, "data", 4)) {
            data = b;
            dlen = n;
            if (pos + 8 + dlen > len) dlen = len - pos - 8;
            break;
        }
        pos += 8 + (size_t) n + (n & 1);
    }
    int ok_fmt = data && ch > 0 && rate >= 8000 && rate <= 384000 &&
                 ((tag == 1 && (bits == 16 || bits == 24 || bits == 32)) || (tag == 3 && bits == 32));
    if (!ok_fmt) { warn(TT(T_WAV_FORMAT), path); free(d); return 0; }
    int bps = bits / 8;
    long long frames = (long long) (dlen / ((size_t) bps * ch));
    s->wav = malloc(sizeof(float) * (size_t) (frames ? frames : 1));
    if (!s->wav) { free(d); return 0; }
    for (long long i = 0; i < frames; i++) {
        float acc = 0;
        for (int c = 0; c < ch; c++) acc += wa_sample(data + ((size_t) i * ch + c) * bps, bits, tag == 3);
        s->wav[i] = acc / ch;
    }
    s->wav_n = frames;
    s->rate = rate;
    s->ch = ch;
    free(d);
    return 1;
}

// spec：loopback・loopback:名前・mic・mic:名前・wav:ファイル
static int src_open(Source *s, const char *spec) {
    memset(s, 0, sizeof *s);
    s->keepalive = 1;
    const char *colon = strchr(spec, ':');
    size_t kl = colon ? (size_t) (colon - spec) : strlen(spec);
    const char *needle = colon ? colon + 1 : NULL;
    if (kl == 3 && !strncmp(spec, "wav", 3) && needle && *needle) {
        s->kind = SRC_WAV;
        snprintf(s->name, sizeof s->name, "%s", needle);
        if (!wav_load(s, needle)) return 0;
        return src_alloc(s);
    }
    if (kl == 8 && !strncmp(spec, "loopback", 8)) s->kind = SRC_LOOPBACK;
    else if (kl == 3 && !strncmp(spec, "mic", 3)) s->kind = SRC_MIC;
    else { warn(TT(T_BAD_SOURCE), spec); return 0; }
    s->dev = wa_find(s->kind == SRC_LOOPBACK ? eRender : eCapture, needle);
    if (!s->dev) { warn(TT(s->kind == SRC_LOOPBACK ? T_NO_LOOPBACK : T_NO_MIC), needle ? needle : "default"); return 0; }
    wa_name(s->dev, s->name, sizeof s->name);
    HRESULT h;
    if (FAILED(h = IMMDevice_Activate(s->dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **) &s->ac)) ||
        FAILED(h = IAudioClient_GetMixFormat(s->ac, &s->wf))) {
        warn(TT(T_DEV_OPEN_FAIL), s->name, (unsigned long) h);
        return 0;
    }
    s->rate = (int) s->wf->nSamplesPerSec;
    s->ch = s->wf->nChannels;
    s->bits = s->wf->wBitsPerSample;
    s->is_float = wa_is_float(s->wf);
    s->mask = wa_chmask(s->wf);
    if (!(s->is_float ? (s->bits == 32 || s->bits == 64) : (s->bits == 8 || s->bits == 16 || s->bits == 24 || s->bits == 32))) {
        warn(TT(T_FORMAT_UNSUPPORTED), s->bits);
        return 0;
    }
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | (s->kind == SRC_LOOPBACK ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
    s->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (FAILED(h = IAudioClient_Initialize(s->ac, AUDCLNT_SHAREMODE_SHARED, flags, 5000000, 0, s->wf, NULL)) ||
        FAILED(h = IAudioClient_SetEventHandle(s->ac, s->ev)) ||
        FAILED(h = IAudioClient_GetService(s->ac, &IID_IAudioCaptureClient, (void **) &s->cap))) {
        warn(TT(T_DEV_OPEN_FAIL), s->name, (unsigned long) h);
        return 0;
    }
    REFERENCE_TIME lat = 0;
    if (SUCCEEDED(IAudioClient_GetStreamLatency(s->ac, &lat))) s->latency_ms = lat / 10000.0;
    return src_alloc(s);
}

static int src_start(Source *s, OnChunk cb) {
    s->on_chunk = cb;
    if (s->kind == SRC_WAV) {
        s->th = CreateThread(NULL, 0, src_wav_thread, s, 0, NULL);
        return s->th != NULL;
    }
    HRESULT h = IAudioClient_Start(s->ac);
    if (FAILED(h)) { warn(TT(T_CAPTURE_FAIL), (unsigned long) h); return 0; }
    if (s->kind == SRC_LOOPBACK && s->keepalive) {
        s->have_ka = ka_start(&s->ka, s->dev);
        if (!s->have_ka) warn("%s", TT(T_KEEPALIVE_FAIL));
    }
    s->produced_until = now_s();  // 取り始めから時間を数える（最初の音が届くまでも無音で埋める）
    s->have_pu = 1;
    s->th = CreateThread(NULL, 0, src_wasapi_thread, s, 0, NULL);
    return s->th != NULL;
}

// 2 回呼んでも 1 回だけ止める
static void src_stop(Source *s) {
    if (InterlockedExchange(&s->stopped, 1)) return;
    s->stop = 1;
    if (s->th) { WaitForSingleObject(s->th, 3000); CloseHandle(s->th); s->th = NULL; }
    if (s->ac) IAudioClient_Stop(s->ac);
    if (s->have_ka) ka_stop(&s->ka);
}
