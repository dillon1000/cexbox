using System;
using System.IO;
using System.Text;
using Windows.Storage;

namespace Cexbox
{
    // A small log of what the app itself does (keys, focus, input sent), written to
    // LocalState\logs\app.log so it can be read over the Device Portal.
    static class AppLog
    {
        static readonly object Gate = new object();
        static readonly string Path = System.IO.Path.Combine(ApplicationData.Current.LocalFolder.Path, "logs", "app.log");
        static readonly DateTime Start = DateTime.UtcNow;
        static int _lines;

        public static void Reset()
        {
            try
            {
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path));
                File.WriteAllText(Path, "");
            }
            catch (IOException)
            {
            }
        }

        public static void Write(string message)
        {
            lock (Gate)
            {
                if (++_lines > 2000) return;
                try
                {
                    File.AppendAllText(Path, $"[{(DateTime.UtcNow - Start).TotalSeconds,9:F3}] {message}\r\n", Encoding.UTF8);
                }
                catch (IOException)
                {
                }
            }
        }

        // Shows control characters as escapes.
        public static string Visible(string s)
        {
            var sb = new StringBuilder();
            foreach (char c in s)
            {
                if (c < 0x20 || c == 0x7f) sb.Append("\\x").Append(((int)c).ToString("x2"));
                else sb.Append(c);
            }
            return sb.ToString();
        }
    }
}
