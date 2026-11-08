// camctl2：PC からカメラを操作する項目の表。番号・名前・種類・選択肢の文言は work/camctl/PROTOCOL.md の表と同じ。
// webcam_rx（PC アプリとのパイプの JSON）・camctl_cli（表を出す・試す）・fake_iphone（偽の CAPS）が読む。
// 表を直したら camctl_cli table で work/camctl2/ctl_ids.md の表を作り直す
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { float value; const char *ja, *en; } wc_ctl_label;
typedef struct {
    uint16_t id;
    const char *name;   // 英語の名前（パイプの JSON の name）
    const char *ja;     // 日本語の名前（PROTOCOL.md の表）
    int kind;           // WC_CTL_TOGGLE など（wcam.h と同じ数。ここでは 0 切り替え・1 選ぶ・2 数・3 読むだけ）
    const char *unit;   // パイプの JSON の unit
    const char *key;    // iPhone の設定の鍵（UserDefaults）
    int rebuild;        // 変えるとカメラを作り直す
    const wc_ctl_label *labels;
    int nlabels;
    const char *note;   // 値の意味（表に書く）
} wc_ctl_def;

static const wc_ctl_label WC_L_MODE[] = { { 0, "自動", "Auto" }, { 1, "固定", "Lock" }, { 2, "手動", "Manual" } };
static const wc_ctl_label WC_L_TARGET[] = { { 0, "顔を優先", "Faces first" }, { 1, "画面の真ん中", "Center of the picture" } };
static const wc_ctl_label WC_L_WB[] = { { 0, "自動", "Auto" }, { 1, "固定", "Lock" }, { 2, "色温度", "Temperature" } };
static const wc_ctl_label WC_L_STAB[] = { { 0, "オフ", "Off" }, { 1, "遅れの少ない補正", "Low latency" }, { 2, "標準", "Standard" }, { 3, "シネマティック", "Cinematic" } };
static const wc_ctl_label WC_L_ROT[] = { { 0, "0°", "0°" }, { 90, "90°", "90°" }, { 180, "180°", "180°" }, { 270, "270°", "270°" } };
static const wc_ctl_label WC_L_CROP[] = { { 0, "中央", "Center" }, { 1, "左寄り", "Left" }, { 2, "右寄り", "Right" }, { 3, "顔に合わせる", "Follow the face" }, { 4, "手動", "Manual" } };
static const wc_ctl_label WC_L_THERMAL[] = { { 0, "正常", "Normal" }, { 1, "やや高い", "Warm" }, { 2, "高い", "Hot" }, { 3, "危険", "Critical" } };
#define WC_L(a) a, (int) (sizeof a / sizeof a[0])

static const wc_ctl_def WC_CTL_DEFS[] = {
    { 1, "focus_mode", "ピント", 1, "", "webcamFocus", 0, WC_L(WC_L_MODE), "0 自動・1 固定・2 手動" },
    { 2, "lens_position", "レンズの位置", 2, "", "webcamFocusPos", 0, NULL, 0, "0 近く〜1 遠く。ピントが手動のとき" },
    { 3, "focus_target", "合わせる所", 1, "", "webcamFocusTarget", 0, WC_L(WC_L_TARGET), "0 顔を優先・1 画面の真ん中。ピントが自動のとき" },
    { 4, "exposure_mode", "明るさ", 1, "", "webcamExposure", 0, WC_L(WC_L_MODE), "0 自動・1 固定・2 手動" },
    { 5, "exposure_bias", "露出補正", 2, "EV", "webcamBias", 0, NULL, 0, "EV。明るさが自動か固定のとき" },
    { 6, "iso", "ISO", 2, "", "webcamISO", 0, NULL, 0, "明るさが手動のとき。0 は「今の値から」（範囲の外でも 0 が来る）。対数で見せる" },
    { 7, "shutter_speed", "シャッター速度", 1, "", "webcamShutter", 0, NULL, 0, "分母（100 は 1/100 秒）。選択肢の文言は 1/100 の形。明るさが手動のとき" },
    { 8, "white_balance", "ホワイトバランス", 1, "", "webcamWB", 0, WC_L(WC_L_WB), "0 自動・1 固定・2 色温度" },
    { 9, "color_temperature", "色温度", 2, "K", "webcamWBTemp", 0, NULL, 0, "K（2500〜8000）。ホワイトバランスが色温度のとき" },
    { 10, "zoom", "ズーム", 2, "×", "webcamZoom", 0, NULL, 0, "見え方の倍率（1 は背面の広角）。choices は倍率のボタン（例 0.5×・1×・2×）。対数で見せる" },
    { 11, "torch", "ライト", 0, "", "webcamTorch", 0, NULL, 0, "0 消す・1 点ける（接続している間だけ点く）" },
    { 12, "torch_level", "ライトの明るさ", 2, "", "webcamTorchLevel", 0, NULL, 0, "0.05〜1（1 が 100%）" },
    { 13, "stabilization", "手ぶれ補正", 1, "", "webcamStab", 1, WC_L(WC_L_STAB), "0 オフ・1 遅れの少ない補正・2 標準・3 シネマティック" },
    { 14, "background_blur", "背景をぼかす", 0, "", "webcamCinematic", 1, NULL, 0, "0・1（シネマティックビデオ。30 fps まで）" },
    { 15, "blur_aperture", "ぼかしの強さ", 2, "F", "webcamAperture", 0, NULL, 0, "F 値。小さいほど強くぼける" },
    { 16, "center_stage", "センターフレーム", 0, "", "webcamCenterStage", 1, NULL, 0, "0・1" },
    { 17, "mirror", "左右反転", 0, "", "webcamMirror", 1, NULL, 0, "0・1" },
    { 18, "flip_vertical", "上下反転", 0, "", "webcamFlipV", 1, NULL, 0, "0・1" },
    { 19, "rotation", "回転", 1, "°", "webcamRotation", 1, WC_L(WC_L_ROT), "0・90・180・270。90 と 270 は縦長（幅と高さが入れ替わる）" },
    { 20, "crop", "切り抜く", 0, "", "webcamCrop", 1, NULL, 0, "0・1" },
    { 21, "crop_position", "切り抜く位置", 1, "", "webcamCropPos", 1, WC_L(WC_L_CROP), "0 中央・1 左寄り・2 右寄り・3 顔に合わせる・4 手動（3 の文言は iPhone のアプリにそろえた。PROTOCOL.md の表では「顔」）" },
    { 22, "crop_size", "切り抜く大きさ", 2, "", "webcamCropSize", 1, NULL, 0, "0.3〜1（映像の幅に対する割合）" },
    { 23, "crop_x", "切り抜く位置の横", 2, "", "webcamCropX", 0, NULL, 0, "0〜1（左が 0）。位置が手動のとき" },
    { 24, "crop_y", "切り抜く位置の縦", 2, "", "webcamCropY", 0, NULL, 0, "0〜1（上が 0）。位置が手動のとき" },
    { 25, "keep_level", "水平を保つ", 0, "", "webcamLevel", 1, NULL, 0, "0・1（背面のカメラだけ）" },
    { 26, "screen_light", "画面のライト", 0, "", "（画面の状態）", 0, NULL, 0, "0・1。iPhone の画面を白く明るくする（アプリが前にいるときだけ点く）" },
    { 27, "screen_light_warm", "画面のライトを暖かい色に", 0, "", "webcamScreenWarm", 0, NULL, 0, "0・1" },
    { 28, "thermal_fps", "熱いときにコマ数を下げる", 0, "", "webcamThermalFps", 0, NULL, 0, "0・1" },
    { 29, "dim_screen", "画面を暗くする", 0, "", "（画面の状態）", 0, NULL, 0, "0・1。iPhone の画面を黒くして明るさを最小にする（カメラとマイクは続く）" },
    { 100, "torch_active", "ライトが点いている", 3, "", "isTorchActive", 0, NULL, 0, "0・1" },
    { 101, "torch_level_now", "ライトの今の明るさ", 3, "", "torchLevel", 0, NULL, 0, "0〜1（熱いと iOS が下げる）" },
    { 102, "thermal_state", "温度の状態", 3, "", "thermalState", 0, WC_L(WC_L_THERMAL), "0 正常・1 やや高い・2 高い・3 危険" },
    { 103, "portrait_enabled", "ポートレートが入っている", 3, "", "isPortraitEffectEnabled", 0, NULL, 0, "0・1（コントロールセンターで入れる）" },
    { 104, "portrait_active", "ポートレートが効いている", 3, "", "isPortraitEffectActive", 0, NULL, 0, "0・1" },
    { 105, "studio_light", "スタジオ照明", 3, "", "isStudioLightEnabled", 0, NULL, 0, "0・1（コントロールセンターで入れる）" },
    { 106, "background_blur_active", "背景ぼかしが効いている", 3, "", "isCinematicVideoCaptureEnabled", 0, NULL, 0, "0・1" },
    { 107, "level_angle", "水平で直している角度", 3, "°", "", 0, NULL, 0, "度" },
    { 108, "fps", "送っているコマ数", 3, "fps", "", 0, NULL, 0, "fps（熱で下げると変わる）" },
};
#define WC_CTL_NDEFS ((int) (sizeof WC_CTL_DEFS / sizeof WC_CTL_DEFS[0]))

static inline const wc_ctl_def *wc_ctl_find(unsigned id) {
    for (int i = 0; i < WC_CTL_NDEFS; i++)
        if (WC_CTL_DEFS[i].id == id) return &WC_CTL_DEFS[i];
    return NULL;
}

static inline const char *wc_ctl_kind_json(int kind) { return kind == 0 ? "toggle" : kind == 1 ? "choice" : kind == 2 ? "range" : ""; }

// 数を JSON の形で書く（float の 7 けたで、余計な 0 を付けない。NaN と無限大は 0）
static inline void wc_ctl_num(char *out, size_t n, float v) {
    if (!(v == v) || v > 3.0e38f || v < -3.0e38f) v = 0;
    if (v == 0) v = 0; // -0 を 0 に
    snprintf(out, n, "%.7g", (double) v);
}

// 選択肢の文言。表に無い値は、シャッター速度（7）は 1/分母、ズームのボタン（10）は 2× の形、それ以外は数
static inline void wc_ctl_label_of(const wc_ctl_def *d, float v, char *ja, char *en, size_t n) {
    if (d)
        for (int i = 0; i < d->nlabels; i++)
            if (d->labels[i].value == v) { snprintf(ja, n, "%s", d->labels[i].ja); snprintf(en, n, "%s", d->labels[i].en); return; }
    char num[32];
    wc_ctl_num(num, sizeof num, v);
    if (d && d->id == 7) { snprintf(ja, n, "1/%s", num); snprintf(en, n, "1/%s", num); return; }
    if (d && d->id == 10) { snprintf(ja, n, "%s×", num); snprintf(en, n, "%s×", num); return; }
    snprintf(ja, n, "%s", num);
    snprintf(en, n, "%s", num);
}
