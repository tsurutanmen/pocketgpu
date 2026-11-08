// 設定を %LOCALAPPDATA%\PocketGPU\settings.json に覚える。中身は平らな辞書（値は文字列・数・真偽・文字列の並び）。
// 試験では PGPU_DATA_DIR で置き場を変える（本物の LOCALAPPDATA に書かないため）。
using System;
using System.Collections;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Web.Script.Serialization;

namespace PocketGPU
{
    static class AppPaths
    {
        public static string DataDir
        {
            get
            {
                string d = Environment.GetEnvironmentVariable("PGPU_DATA_DIR");
                if (string.IsNullOrEmpty(d)) d = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "PocketGPU");
                return d;
            }
        }
        public static string SettingsFile { get { return Path.Combine(DataDir, "settings.json"); } }
        public static string LogDir { get { return Path.Combine(DataDir, "logs"); } }
        public static string RecordFlag { get { return Path.Combine(DataDir, "recording.flag"); } }
        public static string ExeDir { get { return AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\'); } }
        public static string Ensure(string dir) { try { Directory.CreateDirectory(dir); } catch { } return dir; }
    }

    class Settings
    {
        readonly Dictionary<string, object> v = new Dictionary<string, object>();
        public event Action Changed;

        public static Settings Load()
        {
            var s = new Settings();
            try
            {
                string path = AppPaths.SettingsFile;
                if (File.Exists(path))
                {
                    var d = new JavaScriptSerializer().DeserializeObject(File.ReadAllText(path, Encoding.UTF8)) as Dictionary<string, object>;
                    if (d != null) foreach (var kv in d) s.v[kv.Key] = kv.Value;
                }
            }
            catch { }
            return s;
        }

        public string Str(string key, string def)
        {
            object o;
            if (v.TryGetValue(key, out o) && o != null) return Convert.ToString(o, CultureInfo.InvariantCulture);
            return def;
        }
        public bool Bool(string key, bool def)
        {
            object o;
            if (v.TryGetValue(key, out o) && o is bool) return (bool) o;
            return def;
        }
        public int Int(string key, int def)
        {
            object o;
            if (v.TryGetValue(key, out o) && o != null)
            {
                try { return Convert.ToInt32(o, CultureInfo.InvariantCulture); } catch { }
            }
            return def;
        }
        public List<string> List(string key)
        {
            var r = new List<string>();
            object o;
            if (v.TryGetValue(key, out o) && o is IEnumerable && !(o is string))
                foreach (var x in (IEnumerable) o) if (x != null) r.Add(x.ToString());
            return r;
        }

        public void Set(string key, object value)
        {
            object old;
            if (v.TryGetValue(key, out old) && Equals(old, value)) return;
            v[key] = value;
            Save();
            if (Changed != null) Changed();
        }
        public void SetList(string key, List<string> list)
        {
            v[key] = list.ToArray();
            Save();
            if (Changed != null) Changed();
        }

        // 人が読める形で書く。途中で落ちても壊れないよう、別の名前に書いてから置き換える
        public void Save()
        {
            try
            {
                AppPaths.Ensure(AppPaths.DataDir);
                var keys = new List<string>(v.Keys);
                keys.Sort(StringComparer.Ordinal);
                var js = new JavaScriptSerializer();
                var sb = new StringBuilder("{\r\n");
                for (int i = 0; i < keys.Count; i++)
                    sb.Append("  ").Append(js.Serialize(keys[i])).Append(": ").Append(js.Serialize(v[keys[i]])).Append(i + 1 < keys.Count ? ",\r\n" : "\r\n");
                sb.Append("}\r\n");
                string path = AppPaths.SettingsFile, tmp = path + ".tmp";
                File.WriteAllText(tmp, sb.ToString(), new UTF8Encoding(false));
                if (File.Exists(path)) File.Replace(tmp, path, null);
                else File.Move(tmp, path);
            }
            catch { }
        }
    }
}
