using System.Runtime.InteropServices;
using System.Text;

namespace Cexbox
{
    // Calls into xbloader.dll, the native loader built from native/.
    static class Native
    {
        const string Dll = "xbloader.dll";

        public const int NotStarted = 0;
        public const int Starting = 1;
        public const int Running = 2;
        public const int Exited = 3;
        public const int Failed = 4;

        [DllImport(Dll, CharSet = CharSet.Unicode)]
        public static extern int xb_setenv(string name, string value);

        [DllImport(Dll, CharSet = CharSet.Unicode)]
        public static extern int xb_start(string exePath, string commandLine, string workDir, string logDir);

        [DllImport(Dll)]
        public static extern int xb_read_output(byte[] buffer, int capacity);

        [DllImport(Dll)]
        public static extern int xb_write_input(byte[] buffer, int length);

        [DllImport(Dll)]
        public static extern int xb_poll(out int exitCode);

        [DllImport(Dll, CharSet = CharSet.Unicode)]
        public static extern int xb_last_error(StringBuilder buffer, int capacity);
    }
}
