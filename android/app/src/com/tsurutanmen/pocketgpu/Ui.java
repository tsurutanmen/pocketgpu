package com.tsurutanmen.pocketgpu;

import android.content.Context;
import android.content.res.TypedArray;
import android.graphics.Typeface;
import android.net.ConnectivityManager;
import android.net.LinkAddress;
import android.net.LinkProperties;
import android.net.Network;
import android.net.NetworkCapabilities;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

// 画面の部品。色は足さず、端末のテーマの色（文字・薄い文字・区切り線）だけを使う
final class Ui {
    private Ui() {}

    static int dp(Context c, float v) {
        return Math.round(TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, c.getResources().getDisplayMetrics()));
    }

    static int themeColor(Context c, int attr) {
        TypedArray a = c.obtainStyledAttributes(new int[] { attr });
        int v = a.getColor(0, 0xff888888);
        a.recycle();
        return v;
    }

    static TextView title(Context c, String s) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 30);
        t.setTypeface(Typeface.DEFAULT_BOLD);
        t.setTextColor(themeColor(c, android.R.attr.textColorPrimary));
        t.setPadding(0, dp(c, 8), 0, dp(c, 8));
        return t;
    }

    // 区切りの見出し（小さく薄い文字）
    static TextView header(Context c, String s) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        t.setTextColor(themeColor(c, android.R.attr.textColorSecondary));
        t.setPadding(0, dp(c, 24), 0, dp(c, 6));
        return t;
    }

    static View divider(Context c) {
        View v = new View(c);
        TypedArray a = c.obtainStyledAttributes(new int[] { android.R.attr.listDivider });
        v.setBackground(a.getDrawable(0));
        a.recycle();
        v.setLayoutParams(new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, Math.max(1, dp(c, 0.5f))));
        return v;
    }

    // 左に名前・右に値の 1 行。値の TextView を返す
    static TextView row(Context c, LinearLayout parent, String label) {
        LinearLayout r = new LinearLayout(c);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setMinimumHeight(dp(c, 48));
        TextView l = new TextView(c);
        l.setText(label);
        l.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        l.setTextColor(themeColor(c, android.R.attr.textColorPrimary));
        r.addView(l, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        TextView v = new TextView(c);
        v.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        v.setTextColor(themeColor(c, android.R.attr.textColorSecondary));
        v.setGravity(Gravity.END);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
        lp.leftMargin = dp(c, 16);
        r.addView(v, lp);
        parent.addView(r);
        parent.addView(divider(c));
        return v;
    }

    // 左に名前・右に好きな部品の 1 行（温度の点と文字など）
    static void rowWith(Context c, LinearLayout parent, String label, View right) {
        LinearLayout r = new LinearLayout(c);
        r.setOrientation(LinearLayout.HORIZONTAL);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setMinimumHeight(dp(c, 48));
        TextView l = new TextView(c);
        l.setText(label);
        l.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        l.setTextColor(themeColor(c, android.R.attr.textColorPrimary));
        r.addView(l, new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f));
        r.addView(right, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        parent.addView(r);
        parent.addView(divider(c));
    }

    // row の下に足す小さな行（右寄せ・薄い文字）。区切り線の前に入れる
    static TextView subRow(Context c, LinearLayout parent, TextView rowValue) {
        TextView t = new TextView(c);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13);
        t.setTextColor(themeColor(c, android.R.attr.textColorSecondary));
        t.setGravity(Gravity.END);
        t.setPadding(0, 0, 0, dp(c, 8));
        t.setVisibility(View.GONE);
        View row = (View) rowValue.getParent();
        parent.addView(t, parent.indexOfChild(row) + 1);
        return t;
    }

    static TextView note(Context c, String s) {
        TextView t = new TextView(c);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        t.setTextColor(themeColor(c, android.R.attr.textColorSecondary));
        t.setPadding(0, dp(c, 6), 0, dp(c, 6));
        return t;
    }

    // コマンドやログ。等幅で、選んでコピーできる
    static TextView mono(Context c, float sp) {
        TextView t = new TextView(c);
        t.setTypeface(Typeface.MONOSPACE);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setTextIsSelectable(true);
        t.setTextColor(themeColor(c, android.R.attr.textColorPrimary));
        t.setPadding(0, dp(c, 4), 0, dp(c, 4));
        return t;
    }

    // Wi-Fi の IPv4 の住所。Wi-Fi につながっていなければ null
    @SuppressWarnings("deprecation")
    static String wifiAddress(Context c) {
        ConnectivityManager cm = c.getSystemService(ConnectivityManager.class);
        if (cm == null) return null;
        for (Network n : cm.getAllNetworks()) {
            NetworkCapabilities nc = cm.getNetworkCapabilities(n);
            if (nc == null || !nc.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)) continue;
            LinkProperties lp = cm.getLinkProperties(n);
            if (lp == null) continue;
            for (LinkAddress a : lp.getLinkAddresses()) {
                if (a.getAddress() instanceof java.net.Inet4Address) return a.getAddress().getHostAddress();
            }
        }
        return null;
    }
}
