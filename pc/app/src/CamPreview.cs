// カメラのプレビュー：webcam_rx --shm が書く共有メモリ Local\pocketgpu-webcam-v1（camset\pc\wcam_shm.h）を読むだけ。
// 仮想カメラ（dshow_cam.dll）と同じ読み方：頭の seq の枠を写し、写し終えて枠の seq が変わっていなければ使う。
// NV12 を縮めながら BGRA に直して WriteableBitmap に描く。書く側が 1 秒より古ければ「止まっている」とみなす。
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace PocketGPU
{
    class CamPreview : IDisposable
    {
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] static extern IntPtr OpenFileMapping(uint access, bool inherit, string name);
        [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr MapViewOfFile(IntPtr h, uint access, uint hi, uint lo, UIntPtr bytes);
        [DllImport("kernel32.dll")] static extern bool UnmapViewOfFile(IntPtr p);
        [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
        const uint FILE_MAP_READ = 4;
        // 試験用：PGPU_TEST_SHM=名前 で別の共有メモリを読む（ほかの PocketGPU の webcam_rx が書く本物の絵を読まない）
        static readonly string Name = string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PGPU_TEST_SHM")) ? "Local\\pocketgpu-webcam-v1" : Environment.GetEnvironmentVariable("PGPU_TEST_SHM");
        const uint Magic = 0x31435749;

        IntPtr map, view;
        long lastSeq;
        WriteableBitmap bmp;
        byte[] buf;
        public int Width, Height, Fps;
        public long Frames;
        public DateTime LastFrame = DateTime.MinValue;

        static long NowUs() { return (long) (Stopwatch.GetTimestamp() * 1e6 / Stopwatch.Frequency); }

        bool Open()
        {
            if (view != IntPtr.Zero) return true;
            map = OpenFileMapping(FILE_MAP_READ, false, Name);
            if (map == IntPtr.Zero) return false;
            view = MapViewOfFile(map, FILE_MAP_READ, 0, 0, UIntPtr.Zero);
            if (view == IntPtr.Zero) { CloseHandle(map); map = IntPtr.Zero; return false; }
            if ((uint) Marshal.ReadInt32(view, 0) != Magic) { Close(); return false; }
            return true;
        }

        public void Close()
        {
            if (view != IntPtr.Zero) UnmapViewOfFile(view);
            if (map != IntPtr.Zero) CloseHandle(map);
            view = map = IntPtr.Zero;
        }
        public void Dispose() { Close(); }

        // 最後に書き終えたコマの番号（共有メモリが無ければ 0）
        public long CurrentSeq { get { return Open() ? Marshal.ReadInt64(view, 32) : 0; } }

        // 共有メモリを書いているプロセス（試験の記録用）
        public int WriterPid { get { return Open() ? Marshal.ReadInt32(view, 28) : 0; } }

        // 書く側が生きているか（heartbeat が 1 秒以内）
        public bool WriterAlive
        {
            get
            {
                if (!Open()) return false;
                long hb = Marshal.ReadInt64(view, 40);
                if (hb == 0) return false;
                long age = NowUs() - hb;
                return age >= -1000000 && age < 1000000;
            }
        }

        // 新しいコマがあれば描いた絵を返す。無ければ null（前の絵のまま）
        public unsafe BitmapSource Poll(int maxW)
        {
            if (!Open()) return null;
            byte* b = (byte*) view;
            long seq = *(long*) (b + 32);
            if (seq <= 0 || seq == lastSeq) return null;
            uint headBytes = *(uint*) (b + 8), slotBytes = *(uint*) (b + 12), nslots = *(uint*) (b + 16);
            if (nslots == 0 || nslots > 8) return null;
            int slot = (int) (seq % nslots);
            byte* sh = b + 48 + 32 * slot;
            if (*(long*) sh != seq) return null;
            int w = (int) *(uint*) (sh + 8), h = (int) *(uint*) (sh + 12);
            int fps = (int) *(uint*) (sh + 16);
            if (w <= 0 || h <= 0 || w > 1920 || h > 1080 || (long) w * h * 3 / 2 > slotBytes) return null;
            byte* y = b + headBytes + (long) slot * slotBytes;
            byte* uv = y + (long) w * h;

            // 縮める倍率（整数）。プレビューは 336 幅なので、2 倍の画面でもぼやけない程度に
            int step = Math.Max(1, w / maxW);
            int ow = w / step, oh = h / step;
            if (buf == null || buf.Length != ow * oh * 4) buf = new byte[ow * oh * 4];
            fixed (byte* o = buf)
            {
                for (int j = 0; j < oh; j++)
                {
                    int sy = j * step;
                    byte* yr = y + (long) sy * w;
                    byte* uvr = uv + (long) (sy >> 1) * w;
                    byte* op = o + j * ow * 4;
                    for (int i = 0; i < ow; i++)
                    {
                        int sx = i * step;
                        int Y = yr[sx] - 16, U = uvr[sx & ~1] - 128, V = uvr[(sx & ~1) + 1] - 128;
                        int c = 298 * Y;
                        int r = (c + 409 * V + 128) >> 8, g = (c - 100 * U - 208 * V + 128) >> 8, bl = (c + 516 * U + 128) >> 8;
                        op[0] = (byte) (bl < 0 ? 0 : bl > 255 ? 255 : bl);
                        op[1] = (byte) (g < 0 ? 0 : g > 255 ? 255 : g);
                        op[2] = (byte) (r < 0 ? 0 : r > 255 ? 255 : r);
                        op[3] = 255;
                        op += 4;
                    }
                }
            }
            // 写している間に書き換えられていたら捨てる
            if (*(long*) sh != seq) return null;
            lastSeq = seq;
            Width = w; Height = h; Fps = fps;
            Frames++;
            LastFrame = DateTime.Now;
            if (bmp == null || bmp.PixelWidth != ow || bmp.PixelHeight != oh) bmp = new WriteableBitmap(ow, oh, 96, 96, PixelFormats.Bgra32, null);
            bmp.WritePixels(new Int32Rect(0, 0, ow, oh), buf, ow * 4, 0);
            return bmp;
        }
    }
}
