// 窓の絵を H.264 でほどく（Media Foundation の Microsoft H.264 デコーダ・遅れの少ない設定）。
// app/vk/icd/icd.c の vdec_* をそのまま写して、DXVK 用の確かめと記録（say）だけ外した物。GPU（D3D11 の DXVA）でほどけなければソフトでほどく。
#pragma once
#include <d3d11.h>
#include <d3d10.h>
#include <dxgi.h>

#define VDEC_NSTG 4

struct vdec {
    IMFTransform *t;
    IMFDXGIDeviceManager *mgr; // GPU でほどくとき（D3D11 の DXVA）
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    ID3D11Texture2D *stg[VDEC_NSTG]; // 読み出し用（ほどけた絵を順に写しておく）。cam4k：4 つにして、4K の 60 コマ/秒では読み出しを 1 コマ遅らせ、1 回で 2 つ出た絵も捨てずに渡せるようにした
    int cur_stg;               // 最後に写した物
    int mapped_stg;            // いま Map している物
    struct { int stg; uint64_t tag; } outq[VDEC_NSTG]; // 取り出し待ちの絵（古い順）
    int outq_n;
    uint64_t n_none, n_multi;  // 記録用：入力しても絵が出なかった回数・絵が 2 つ以上出た回数
    uint32_t stg_gen;          // stg を作り直すたびに増える（遅らせて持っている絵が、作り直しで消えた stg を指していないかを見る）
    uint32_t sw, sh;
    int hw, staged, mapped;
    int provides;           // 出力の sample をデコーダが用意する
    DWORD outsz;
    uint32_t fw, fh, pitch; // 出てくる NV12 の大きさ（16 の倍数に揃っていることがある）・1 行のバイト
    uint8_t *nv12;          // 最後にほどけた絵
    size_t cap;
    uint32_t npitch, nrows; // nv12 の 1 行のバイトと Y の面の行数
    int has;
    uint64_t tag;           // 最後にほどけた絵の札（何回目の present の物か）
    char adapter[128];      // dec1013：GPU でほどいているアダプタの名前（記録用。ソフトなら空）
    int adapter_missing;    // dec1013：アダプタの名前を指定されたが見つからず、Windows の既定を使った
};

static void vdec_unmap(struct vdec *d);

static void vdec_close(struct vdec *d) {
    if (d->t) {
        IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        IMFTransform_Release(d->t);
    }
    if (d->mgr) IMFDXGIDeviceManager_Release(d->mgr);
    vdec_unmap(d);
    for (int i = 0; i < VDEC_NSTG; i++) if (d->stg[i]) ID3D11Texture2D_Release(d->stg[i]);
    if (d->ctx) ID3D11DeviceContext_Release(d->ctx);
    if (d->dev) ID3D11Device_Release(d->dev);
    free(d->nv12);
    memset(d, 0, sizeof *d);
}

// 出す形を NV12 に決める。大きさと 1 行のバイトを覚える
static int vdec_out_type(struct vdec *d) {
    for (DWORD i = 0;; i++) {
        IMFMediaType *t = NULL;
        if (FAILED(IMFTransform_GetOutputAvailableType(d->t, 0, i, &t)) || !t) return 0;
        GUID sub;
        if (SUCCEEDED(IMFMediaType_GetGUID(t, &VG_SUBTYPE, &sub)) && !memcmp(&sub, &VG_NV12, sizeof sub)) {
            HRESULT hr = IMFTransform_SetOutputType(d->t, 0, t, 0);
            UINT64 fs = 0;
            UINT32 st = 0;
            IMFMediaType_GetUINT64(t, &VG_FRAME_SIZE, &fs);
            d->fw = (uint32_t) (fs >> 32), d->fh = (uint32_t) fs;
            d->pitch = SUCCEEDED(IMFMediaType_GetUINT32(t, &VG_STRIDE, &st)) && (int32_t) st > 0 ? st : d->fw;
            IMFMediaType_Release(t);
            if (FAILED(hr)) return 0;
            MFT_OUTPUT_STREAM_INFO si;
            memset(&si, 0, sizeof si);
            IMFTransform_GetOutputStreamInfo(d->t, 0, &si);
            d->provides = (si.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
            d->outsz = si.cbSize > d->pitch * d->fh * 3 / 2 ? si.cbSize : d->pitch * d->fh * 3 / 2;
            return 1;
        }
        IMFMediaType_Release(t);
    }
}

// dec1013：GPU でほどくアダプタを名前の一部で選ぶ（webcam_rx の --adapter。例 Intel）。NULL か空なら Windows の既定
static const char *g_vdec_adapter;

static void vdec_narrow(const WCHAR *w, char *out, size_t n) {
    size_t k = 0;
    for (size_t i = 0; w[i] && k + 1 < n; i++) out[k++] = w[i] < 128 ? (char) w[i] : '?';
    out[k] = 0;
}

static int vdec_icontains(const char *hay, const char *needle) {
    size_t nl = strlen(needle);
    if (!nl) return 1;
    for (size_t i = 0; hay[i]; i++) {
        size_t k = 0;
        while (k < nl && hay[i + k]) {
            char a = hay[i + k], b = needle[k];
            if (a >= 'A' && a <= 'Z') a = (char) (a + 32);
            if (b >= 'A' && b <= 'Z') b = (char) (b + 32);
            if (a != b) break;
            k++;
        }
        if (k == nl) return 1;
    }
    return 0;
}

// g_vdec_adapter に合う名前のアダプタ（呼ぶ側が Release）。指定が無い・見つからなければ NULL
static IDXGIAdapter1 *vdec_pick_adapter(void) {
    static const GUID fac_g = { 0x770aae78, 0xf26f, 0x4dba, { 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 } }; // IDXGIFactory1
    if (!g_vdec_adapter || !*g_vdec_adapter) return NULL;
    WCHAR path[MAX_PATH + 16];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH) return NULL;
    wcscpy(path + n, L"\\dxgi.dll");
    HMODULE m = LoadLibraryW(path);
    if (!m) return NULL;
    HRESULT (WINAPI *mk)(const GUID *, void **) = (HRESULT (WINAPI *)(const GUID *, void **)) (void *) GetProcAddress(m, "CreateDXGIFactory1");
    IDXGIFactory1 *f = NULL;
    if (!mk || FAILED(mk(&fac_g, (void **) &f)) || !f) return NULL;
    IDXGIAdapter1 *found = NULL;
    // camgpu1015：名前が igpu のときは、実物のアダプタが 2 つ以上ある PC で、専用のメモリがいちばん少ない物（内蔵の GPU）を選ぶ。
    // 1 つしかない PC（この PC は 3060 だけ）では選ばず、Windows の既定のまま
    if (!_stricmp(g_vdec_adapter, "igpu")) {
        UINT hw = 0;
        SIZE_T least = (SIZE_T) -1;
        UINT best = 0;
        for (UINT i = 0;; i++) {
            IDXGIAdapter1 *a = NULL;
            DXGI_ADAPTER_DESC1 ad;
            if (FAILED(IDXGIFactory1_EnumAdapters1(f, i, &a)) || !a) break;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &ad)) && !(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                hw++;
                if (ad.DedicatedVideoMemory < least) { least = ad.DedicatedVideoMemory; best = i; }
            }
            IDXGIAdapter1_Release(a);
        }
        if (hw >= 2) IDXGIFactory1_EnumAdapters1(f, best, &found);
        IDXGIFactory1_Release(f);
        return found;
    }
    for (UINT i = 0;; i++) {
        IDXGIAdapter1 *a = NULL;
        if (FAILED(IDXGIFactory1_EnumAdapters1(f, i, &a)) || !a) break;
        DXGI_ADAPTER_DESC1 ad;
        char nm[128];
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &ad))) {
            vdec_narrow(ad.Description, nm, sizeof nm);
            if (vdec_icontains(nm, g_vdec_adapter)) { found = a; break; }
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    return found;
}

// 作った装置がどのアダプタの物かの名前（記録用）
static void vdec_dev_adapter_name(ID3D11Device *dev, char *out, size_t n) {
    static const GUID dxgi_dev_g = { 0x54ec77fa, 0x1377, 0x44e6, { 0x8c, 0x32, 0x88, 0xfd, 0x5f, 0x44, 0xc8, 0x4c } }; // IDXGIDevice
    out[0] = 0;
    IDXGIDevice *dd = NULL;
    IDXGIAdapter *a = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &dxgi_dev_g, (void **) &dd)) && dd) {
        if (SUCCEEDED(IDXGIDevice_GetAdapter(dd, &a)) && a) {
            DXGI_ADAPTER_DESC ad;
            if (SUCCEEDED(IDXGIAdapter_GetDesc(a, &ad))) vdec_narrow(ad.Description, out, n);
            IDXGIAdapter_Release(a);
        }
        IDXGIDevice_Release(dd);
    }
}

// GPU でほどけるようにする（D3D11 の装置を作り、デコーダに渡す）。だめなら 0 で、ソフトでほどく。
// d3d11.dll は Windows の物をパスで読む
static int vdec_hw(struct vdec *d) {
    static const GUID aware_g = { 0x206b4fc8, 0xfcf9, 0x4c51, { 0xaf, 0xe3, 0x97, 0x64, 0x36, 0x9e, 0x33, 0xa0 } };       // MF_SA_D3D11_AWARE
    static const GUID mt_g = { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };          // ID3D10Multithread
    static PFN_D3D11_CREATE_DEVICE create;
    static HRESULT (WINAPI *mk_mgr)(UINT *, IMFDXGIDeviceManager **);
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    const char *e = getenv("IPHONE_VK_VIDEO_HWDEC");
    if (e && *e == '0') return 0;
    BOOL pending = FALSE;
    if (InitOnceBeginInitialize(&once, 0, &pending, NULL) && pending) {
        WCHAR path[MAX_PATH + 16];
        UINT n = GetSystemDirectoryW(path, MAX_PATH);
        HMODULE m = NULL, mf = GetModuleHandleW(L"mfplat.dll");
        if (n && n < MAX_PATH) { wcscpy(path + n, L"\\d3d11.dll"); m = LoadLibraryW(path); }
        if (m) create = (PFN_D3D11_CREATE_DEVICE) (void *) GetProcAddress(m, "D3D11CreateDevice");
        if (mf) mk_mgr = (HRESULT (WINAPI *)(UINT *, IMFDXGIDeviceManager **)) (void *) GetProcAddress(mf, "MFCreateDXGIDeviceManager");
        InitOnceComplete(&once, 0, NULL);
    }
    if (!create || !mk_mgr) return 0;
    IMFAttributes *at = NULL;
    UINT32 aware = 0;
    if (SUCCEEDED(IMFTransform_GetAttributes(d->t, &at)) && at) {
        IMFAttributes_GetUINT32(at, &aware_g, &aware);
        IMFAttributes_Release(at);
    }
    // dec1013：アダプタの指定があれば、その物で作る（見つからなければ Windows の既定）
    IDXGIAdapter1 *pick = aware ? vdec_pick_adapter() : NULL;
    d->adapter_missing = aware && g_vdec_adapter && *g_vdec_adapter && !pick;
    HRESULT chr = aware ? create((IDXGIAdapter *) pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_VIDEO_SUPPORT, NULL, 0, D3D11_SDK_VERSION, &d->dev, NULL, NULL) : E_FAIL;
    if (pick) IDXGIAdapter1_Release(pick);
    if (!aware || FAILED(chr)) {
        d->dev = NULL;
        return 0;
    }
    vdec_dev_adapter_name(d->dev, d->adapter, sizeof d->adapter);
    ID3D11Device_GetImmediateContext(d->dev, &d->ctx);
    ID3D10Multithread *mt = NULL;
    if (SUCCEEDED(ID3D11Device_QueryInterface(d->dev, &mt_g, (void **) &mt)) && mt) {
        ID3D10Multithread_SetMultithreadProtected(mt, TRUE);
        ID3D10Multithread_Release(mt);
    }
    UINT tok = 0;
    if (FAILED(mk_mgr(&tok, &d->mgr)) || FAILED(IMFDXGIDeviceManager_ResetDevice(d->mgr, (IUnknown *) d->dev, tok)) ||
        FAILED(IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_SET_D3D_MANAGER, (ULONG_PTR) d->mgr))) {
        if (d->mgr) IMFDXGIDeviceManager_Release(d->mgr);
        if (d->ctx) ID3D11DeviceContext_Release(d->ctx);
        ID3D11Device_Release(d->dev);
        d->mgr = NULL, d->dev = NULL, d->ctx = NULL;
        d->adapter[0] = 0;
        return 0;
    }
    return 1;
}

// hw なら GPU でほどけるかも試す（IPHONE_VK_VIDEO_HWDEC=0 でソフトだけ）
static int vdec_open(struct vdec *d, int hw) {
    memset(d, 0, sizeof *d);
    if (!vid_mf()) return 0;
    if (FAILED(CoCreateInstance(&VG_CLSID_H264_DEC, NULL, CLSCTX_INPROC_SERVER, &VG_IID_TRANSFORM, (void **) &d->t))) { d->t = NULL; return 0; }
    // 遅れの少ない設定：ほどけたらすぐ出す（並べ替えのために待たない）
    IMFAttributes *at = NULL;
    if (SUCCEEDED(IMFTransform_GetAttributes(d->t, &at)) && at) {
        IMFAttributes_SetUINT32(at, &VG_LOW_LATENCY, TRUE);
        IMFAttributes_Release(at);
    }
    ICodecAPI *api = NULL;
    if (SUCCEEDED(IMFTransform_QueryInterface(d->t, &VG_IID_CODECAPI, (void **) &api)) && api) {
        vid_api_bool(api, &VG_LOW_LATENCY, 1);
        ICodecAPI_Release(api);
    }
    d->hw = hw && vdec_hw(d);
    // camgpu1015：ソフトでほどくときの糸の数（環境変数 POCKETGPU_DEC_THREADS。無ければデコーダの既定）。CODECAPI_AVDecNumWorkerThreads
    if (!d->hw) {
        char tv[8];
        if (GetEnvironmentVariableA("POCKETGPU_DEC_THREADS", tv, sizeof tv) > 0 && atoi(tv) > 0) {
            static const GUID g_threads = { 0x9561c3e8, 0xea9e, 0x4435, { 0x9b, 0x1e, 0xa9, 0x3e, 0x69, 0x18, 0x94, 0xd3 } };
            ICodecAPI *api2 = NULL;
            if (SUCCEEDED(IMFTransform_QueryInterface(d->t, &VG_IID_CODECAPI, (void **) &api2)) && api2) {
                VARIANT x;
                VariantInit(&x);
                x.vt = VT_UI4;
                x.ulVal = (ULONG) atoi(tv);
                ICodecAPI_SetValue(api2, &g_threads, &x);
                ICodecAPI_Release(api2);
            }
        }
    }
    IMFMediaType *in = vid_type(&VG_H264, 0, 0, 0, 0, 0);
    HRESULT hr = in ? IMFTransform_SetInputType(d->t, 0, in, 0) : E_FAIL;
    if (in) IMFMediaType_Release(in);
    if (FAILED(hr)) { vdec_close(d); return 0; }
    vdec_out_type(d); // 絵の大きさが分かるのは最初のコマから（そのとき形が変わったと言ってくる）
    IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    IMFTransform_ProcessMessage(d->t, MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    return 1;
}

// ソフトでほどいた絵（CPU のメモリ）を nv12 に写す
static int vdec_take_cpu(struct vdec *d, IMFMediaBuffer *mb) {
    IMF2DBuffer *b2 = NULL;
    BYTE *p = NULL;
    LONG pitch = 0;
    DWORD len = 0;
    int locked2d = SUCCEEDED(IMFMediaBuffer_QueryInterface(mb, &VG_IID_2DBUFFER, (void **) &b2)) && b2 && SUCCEEDED(IMF2DBuffer_Lock2D(b2, &p, &pitch));
    if (!locked2d && SUCCEEDED(IMFMediaBuffer_Lock(mb, &p, NULL, &len))) pitch = (LONG) d->pitch;
    int ok = 0;
    if (p && pitch > 0) {
        size_t need = (size_t) pitch * d->fh * 3 / 2;
        if (!locked2d && len < need) need = 0;
        if (need && d->cap < need) {
            free(d->nv12);
            d->nv12 = malloc(need);
            d->cap = d->nv12 ? need : 0;
        }
        if (need && d->nv12) {
            memcpy(d->nv12, p, need);
            d->npitch = (uint32_t) pitch, d->nrows = d->fh, d->has = 1, d->staged = 0, ok = 1;
        }
    }
    if (locked2d) IMF2DBuffer_Unlock2D(b2);
    else if (p) IMFMediaBuffer_Unlock(mb);
    if (b2) IMF2DBuffer_Release(b2);
    return ok;
}

// GPU でほどいた絵（D3D11 の画像）を、読み出し用の画像に写す（GPU の中で・待たない）。CPU に読み出すのは描くコマだけ
static int vdec_take_gpu(struct vdec *d, IMFMediaBuffer *mb) {
    static const GUID tex_g = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } }; // ID3D11Texture2D
    IMFDXGIBuffer *db = NULL;
    ID3D11Texture2D *tx = NULL;
    UINT sub = 0;
    int ok = 0;
    if (SUCCEEDED(IMFMediaBuffer_QueryInterface(mb, &VG_IID_DXGIBUFFER, (void **) &db)) && db &&
        SUCCEEDED(IMFDXGIBuffer_GetResource(db, &tex_g, (void **) &tx)) && tx && SUCCEEDED(IMFDXGIBuffer_GetSubresourceIndex(db, &sub))) {
        D3D11_TEXTURE2D_DESC td;
        ID3D11Texture2D_GetDesc(tx, &td);
        int have_all = 1;
        for (int i = 0; i < VDEC_NSTG; i++) if (!d->stg[i]) have_all = 0;
        if (td.Format == DXGI_FORMAT_NV12 && (!have_all || d->sw != td.Width || d->sh != td.Height)) {
            vdec_unmap(d);
            for (int i = 0; i < VDEC_NSTG; i++) {
                if (d->stg[i]) ID3D11Texture2D_Release(d->stg[i]);
                d->stg[i] = NULL;
            }
            D3D11_TEXTURE2D_DESC sd;
            memset(&sd, 0, sizeof sd);
            sd.Width = td.Width, sd.Height = td.Height, sd.MipLevels = 1, sd.ArraySize = 1, sd.Format = td.Format;
            sd.SampleDesc.Count = 1, sd.Usage = D3D11_USAGE_STAGING, sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            have_all = 1;
            for (int i = 0; i < VDEC_NSTG; i++)
                if (FAILED(ID3D11Device_CreateTexture2D(d->dev, &sd, NULL, &d->stg[i]))) d->stg[i] = NULL, have_all = 0;
            if (!have_all) {
                for (int i = 0; i < VDEC_NSTG; i++) { if (d->stg[i]) ID3D11Texture2D_Release(d->stg[i]); d->stg[i] = NULL; }
            }
            d->sw = td.Width, d->sh = td.Height, d->stg_gen++, d->cur_stg = 0, d->outq_n = 0;
        }
        if (td.Format == DXGI_FORMAT_NV12 && have_all) {
            const int i = (d->cur_stg + 1) % VDEC_NSTG; // 前に写した物とは別の staging へ（前の絵を CPU が読んでいる間に、GPU が次の絵を写せる）
            ID3D11DeviceContext_CopySubresourceRegion(d->ctx, (ID3D11Resource *) d->stg[i], 0, 0, 0, 0, (ID3D11Resource *) tx, sub, NULL);
            d->cur_stg = i;
            d->has = 1, d->staged = 1, ok = 1;
        }
    }
    if (tx) ID3D11Texture2D_Release(tx);
    if (db) IMFDXGIBuffer_Release(db);
    return ok;
}

// ほどけた絵を受け取って nv12 に写す。1 受け取った・0 もう無い・-1 失敗
static int vdec_pull(struct vdec *d) {
    for (int tries = 0; tries < 4; tries++) {
        MFT_OUTPUT_DATA_BUFFER ob;
        memset(&ob, 0, sizeof ob);
        if (!d->provides && !(ob.pSample = vid_sample(d->outsz))) return -1;
        DWORD st = 0;
        HRESULT hr = IMFTransform_ProcessOutput(d->t, 0, 1, &ob, &st);
        if (ob.pEvents) IMFCollection_Release(ob.pEvents);
        if (hr == MF_E_TRANSFORM_STREAM_CHANGE || hr == MF_E_TRANSFORM_TYPE_NOT_SET) {
            // 最初のコマ・大きさが変わったとき：出す形を決め直して、もう一度
            if (ob.pSample) IMFSample_Release(ob.pSample);
            if (!vdec_out_type(d)) return hr == MF_E_TRANSFORM_TYPE_NOT_SET ? 0 : -1;
            continue;
        }
        if (FAILED(hr)) {
            if (ob.pSample) IMFSample_Release(ob.pSample);
            return hr == MF_E_TRANSFORM_NEED_MORE_INPUT ? 0 : -1;
        }
        int ok = 0;
        IMFMediaBuffer *mb = NULL;
        if (SUCCEEDED(IMFSample_GetBufferByIndex(ob.pSample, 0, &mb)) && mb) {
            ok = (d->hw && vdec_take_gpu(d, mb)) || vdec_take_cpu(d, mb);
            IMFMediaBuffer_Release(mb);
        }
        LONGLONG t = 0;
        if (ok && SUCCEEDED(IMFSample_GetSampleTime(ob.pSample, &t))) d->tag = (uint64_t) t;
        if (ok && d->staged) {
            // 取り出し待ちの列に入れる（1 回の入力で絵が 2 つ以上出ても、捨てずに順に渡せるように）。いっぱいなら一番古いのを捨てる
            if (d->outq_n == VDEC_NSTG) {
                memmove(&d->outq[0], &d->outq[1], sizeof d->outq[0] * (VDEC_NSTG - 1));
                d->outq_n--;
            }
            d->outq[d->outq_n].stg = d->cur_stg, d->outq[d->outq_n].tag = d->tag, d->outq_n++;
        }
        IMFSample_Release(ob.pSample);
        return ok ? 1 : -1;
    }
    return -1;
}

// ほどけた最後の絵の NV12 を指す。GPU でほどいた物はここで読み出す（終わったら vdec_unmap）
// idx：どちらの stg を読むか（ふつうは最後に写した d->cur_stg。読み出しを遅らせたときは、その絵を写した方）
static int vdec_map_stg(struct vdec *d, int idx, const uint8_t **y, uint32_t *pitch, uint32_t *rows) {
    if (!d->has) return 0;
    if (!d->staged) {
        *y = d->nv12, *pitch = d->npitch, *rows = d->nrows;
        return 1;
    }
    if (idx < 0 || idx >= VDEC_NSTG || !d->stg[idx]) return 0;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ID3D11DeviceContext_Map(d->ctx, (ID3D11Resource *) d->stg[idx], 0, D3D11_MAP_READ, 0, &m))) return 0;
    d->mapped = 1, d->mapped_stg = idx;
    *y = (const uint8_t *) m.pData, *pitch = m.RowPitch, *rows = d->sh; // UV の面は Y の面のすぐ後ろ（1 行は同じ長さ）
    return 1;
}
static int vdec_map(struct vdec *d, const uint8_t **y, uint32_t *pitch, uint32_t *rows) { return vdec_map_stg(d, d->cur_stg, y, pitch, rows); }

// cam4k：取り出し待ちの絵を古い順に 1 つ出す（GPU でほどいたときだけ）。*stg はその絵を写した staging（vdec_map_stg に渡す）・*tag は札。無ければ 0
static int vdec_out_pop(struct vdec *d, int *stg, uint64_t *tag) {
    if (d->outq_n <= 0) return 0;
    *stg = d->outq[0].stg, *tag = d->outq[0].tag;
    memmove(&d->outq[0], &d->outq[1], sizeof d->outq[0] * (size_t) (d->outq_n - 1));
    d->outq_n--;
    return 1;
}

static void vdec_unmap(struct vdec *d) {
    if (d->mapped && d->stg[d->mapped_stg]) ID3D11DeviceContext_Unmap(d->ctx, (ID3D11Resource *) d->stg[d->mapped_stg], 0);
    d->mapped = 0;
}

// 1 コマぶんの NAL の並びを入れて、出てきた絵を受け取る。札は sample の時刻として通し、出てきた絵の物を d->tag に入れる。
// 1 ほどけた絵がある・0 まだ無い・-1 失敗
static int vdec_push(struct vdec *d, const uint8_t *au, size_t n, uint64_t tag) {
    IMFSample *s = vid_sample((DWORD) n);
    if (!s) return -1;
    IMFMediaBuffer *b = NULL;
    BYTE *p = NULL;
    if (SUCCEEDED(IMFSample_GetBufferByIndex(s, 0, &b)) && SUCCEEDED(IMFMediaBuffer_Lock(b, &p, NULL, NULL))) {
        memcpy(p, au, n);
        IMFMediaBuffer_Unlock(b);
        IMFMediaBuffer_SetCurrentLength(b, (DWORD) n);
    }
    if (b) IMFMediaBuffer_Release(b);
    IMFSample_SetSampleTime(s, (LONGLONG) tag);
    int got = 0;
    HRESULT hr = IMFTransform_ProcessInput(d->t, 0, s, 0);
    if (hr == MF_E_NOTACCEPTING) {
        // 先に出てきた絵を受け取ってから入れ直す
        int k;
        while ((k = vdec_pull(d)) > 0) got = 1;
        if (k < 0) { IMFSample_Release(s); return -1; }
        hr = IMFTransform_ProcessInput(d->t, 0, s, 0);
    }
    IMFSample_Release(s);
    if (FAILED(hr)) return -1;
    int k, np = 0;
    while ((k = vdec_pull(d)) > 0) got = 1, np++;
    if (np == 0) d->n_none++;
    else if (np >= 2) d->n_multi++; // 1 回の入力で絵が 2 つ以上出た：最後の 1 つしか使わない（前の絵は捨てる）
    return k < 0 ? -1 : got;
}


