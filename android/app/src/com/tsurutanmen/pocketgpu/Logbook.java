package com.tsurutanmen.pocketgpu;

import android.util.Log;

import java.text.SimpleDateFormat;
import java.util.ArrayDeque;
import java.util.Date;
import java.util.Iterator;
import java.util.Locale;

// 画面の「ログ」と logcat（adb logcat -s pocketgpu）の両方に書く。
// 0.1.2 から：日本語と英語の両方を受け取り、画面の言語（アプリの言語）に合わせて書く。前は日本語だけだった（テスターの指摘）
final class Logbook {
    private static final int MAX = 300;
    private static final ArrayDeque<String> lines = new ArrayDeque<>();
    private static final SimpleDateFormat fmt = new SimpleDateFormat("HH:mm:ss", Locale.US);

    private Logbook() {}

    // 画面の言語が日本語か（アプリごとの言語にも従う。Android はアプリの言語をプロセスの既定の言語にする）
    static boolean ja() {
        return "ja".equals(Locale.getDefault().getLanguage());
    }

    // 画面の言語の方を返す。理由の文など、あとでログに入れる文を作るときに使う
    static String t(String ja, String en) {
        return ja() ? ja : en;
    }

    static void i(String ja, String en) {
        String s = t(ja, en);
        Log.i("pocketgpu", s);
        synchronized (lines) {
            lines.addLast(fmt.format(new Date()) + " " + s);
            while (lines.size() > MAX) lines.removeFirst();
        }
    }

    // 新しい方から n 行（並びは古い順）
    static String tail(int n) {
        StringBuilder b = new StringBuilder();
        synchronized (lines) {
            int skip = Math.max(0, lines.size() - n);
            Iterator<String> it = lines.iterator();
            while (it.hasNext()) {
                String s = it.next();
                if (skip-- > 0) continue;
                if (b.length() > 0) b.append('\n');
                b.append(s);
            }
        }
        return b.toString();
    }
}
