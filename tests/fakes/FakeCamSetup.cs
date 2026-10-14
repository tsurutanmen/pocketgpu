// Test double for camera_setup.exe. It never touches the registry.
//   camera_setup.exe install     writes its own full path to the file in PGPU_TEST_CAMERA_REG
//   camera_setup.exe uninstall   deletes that file
using System;
using System.IO;
using System.Reflection;

public static class FakeCamSetup
{
    public static int Main(string[] args)
    {
        string mark = Environment.GetEnvironmentVariable("PGPU_TEST_CAMERA_REG");
        if (string.IsNullOrEmpty(mark) || args.Length < 1) return 2;
        if (args[0] == "install")
        {
            File.WriteAllText(mark, Assembly.GetExecutingAssembly().Location);
            return 0;
        }
        if (args[0] == "uninstall")
        {
            if (File.Exists(mark)) File.Delete(mark);
            return 0;
        }
        return 2;
    }
}
