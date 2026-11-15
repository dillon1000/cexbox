using System;
using System.IO;
using System.Text;
using System.Threading.Tasks;
using Windows.ApplicationModel.Core;
using Windows.Storage;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;

namespace Cexbox
{
    public sealed partial class MainPage : Page
    {
        const int MaxOutputChars = 200000;

        readonly string _root = ApplicationData.Current.LocalFolder.Path;
        readonly Decoder _utf8 = new UTF8Encoding(false).GetDecoder();
        readonly byte[] _readBuffer = new byte[65536];
        readonly char[] _charBuffer = new char[65536 * 2];
        readonly StringBuilder _output = new StringBuilder();
        DispatcherTimer _timer;

        string ClaudeDir => Path.Combine(_root, "claude");
        string ClaudeExe => Path.Combine(ClaudeDir, "claude.exe");
        string HomeDir => Path.Combine(_root, "home");
        string LogDir => Path.Combine(_root, "logs");

        public MainPage()
        {
            InitializeComponent();
            Loaded += OnLoaded;
        }

        void OnLoaded(object sender, RoutedEventArgs e)
        {
            Directory.CreateDirectory(LogDir);
            RefreshStatus();

            // After a crash the app closes, so show what the last run left behind.
            string loaderLog = Path.Combine(LogDir, "loader.log");
            string outputLog = Path.Combine(LogDir, "output.log");
            if (File.Exists(loaderLog))
            {
                Append("Loader log from the last run:\n" + Tail(loaderLog, 12000) + "\n");
            }
            if (File.Exists(outputLog) && new FileInfo(outputLog).Length > 0)
            {
                Append("Program output from the last run:\n" + Tail(outputLog, 8000) + "\n");
            }
        }

        void RefreshStatus()
        {
            if (File.Exists(ClaudeExe))
            {
                string versionFile = Path.Combine(ClaudeDir, "version.txt");
                string version = File.Exists(versionFile) ? File.ReadAllText(versionFile).Trim() : "(version unknown)";
                StatusText.Text = $"Claude Code {version} is downloaded.";
                RunButton.IsEnabled = true;
            }
            else
            {
                StatusText.Text = "Claude Code isn't downloaded yet. You can also copy claude.exe into LocalState\\claude with the Device Portal.";
                RunButton.IsEnabled = false;
            }
        }

        async void Download_Click(object sender, RoutedEventArgs e)
        {
            DownloadButton.IsEnabled = false;
            RunButton.IsEnabled = false;
            Progress.Value = 0;
            Progress.Visibility = Visibility.Visible;
            StatusText.Text = "Downloading Claude Code…";
            try
            {
                string version = await ClaudeDownloader.DownloadAsync(ClaudeDir, (done, total) =>
                {
                    var _ = Dispatcher.RunAsync(Windows.UI.Core.CoreDispatcherPriority.Low, () =>
                    {
                        Progress.Value = total > 0 ? (double)done / total : 0;
                        StatusText.Text = $"Downloading Claude Code… {done / (1024 * 1024)} of {total / (1024 * 1024)} MB";
                    });
                });
                RefreshStatus();
            }
            catch (Exception ex)
            {
                StatusText.Text = "Download failed: " + ex.Message;
                RunButton.IsEnabled = File.Exists(ClaudeExe);
            }
            finally
            {
                Progress.Visibility = Visibility.Collapsed;
                DownloadButton.IsEnabled = true;
            }
        }

        async void Run_Click(object sender, RoutedEventArgs e)
        {
            string local = Path.Combine(HomeDir, "AppData", "Local");
            string roaming = Path.Combine(HomeDir, "AppData", "Roaming");
            string temp = Path.Combine(local, "Temp");
            Directory.CreateDirectory(roaming);
            Directory.CreateDirectory(temp);
            Directory.CreateDirectory(LogDir);

            Native.xb_setenv("USERPROFILE", HomeDir);
            Native.xb_setenv("HOME", HomeDir);
            Native.xb_setenv("APPDATA", roaming);
            Native.xb_setenv("LOCALAPPDATA", local);
            Native.xb_setenv("TEMP", temp);
            Native.xb_setenv("TMP", temp);
            Native.xb_setenv("DISABLE_AUTOUPDATER", "1");

            string commandLine = "\"" + ClaudeExe + "\" " + ArgsBox.Text.Trim();
            _output.Clear();
            OutputText.Text = "";
            Append("> claude " + ArgsBox.Text.Trim() + "\n");

            RunButton.IsEnabled = false;
            DownloadButton.IsEnabled = false;
            RestartButton.Visibility = Visibility.Visible;
            StatusText.Text = "Loading claude.exe…";

            // Loading reads the whole 370 MB file, so keep it off the UI thread.
            string exe = ClaudeExe, home = HomeDir, logs = LogDir;
            int rc = await Task.Run(() => Native.xb_start(exe, commandLine, home, logs));
            if (rc != 0)
            {
                Append("Could not start: " + LastError() + "\n");
                StatusText.Text = "Claude Code didn't start. The loader log in LocalState\\logs has details.";
                return;
            }
            SendButton.IsEnabled = true;
            StatusText.Text = "Running. Only one run per app launch for now; restart the app to run again.";

            _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(100) };
            _timer.Tick += (s, args) => Pump();
            _timer.Start();
        }

        void Pump()
        {
            int n;
            while ((n = Native.xb_read_output(_readBuffer, _readBuffer.Length)) > 0)
            {
                int chars = _utf8.GetChars(_readBuffer, 0, n, _charBuffer, 0, false);
                Append(new string(_charBuffer, 0, chars));
            }

            int state = Native.xb_poll(out int exitCode);
            if (state == Native.Exited || state == Native.Failed)
            {
                // Drain anything that arrived between the read and the poll.
                while ((n = Native.xb_read_output(_readBuffer, _readBuffer.Length)) > 0)
                {
                    int chars = _utf8.GetChars(_readBuffer, 0, n, _charBuffer, 0, false);
                    Append(new string(_charBuffer, 0, chars));
                }
                _timer.Stop();
                SendButton.IsEnabled = false;
                Append(state == Native.Exited ? $"\nExited with code {exitCode}.\n" : "\nFailed: " + LastError() + "\n");
                StatusText.Text = "Finished. Restart the app to run again.";
            }
        }

        void Send_Click(object sender, RoutedEventArgs e)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(InputBox.Text + "\n");
            if (Native.xb_write_input(bytes, bytes.Length) < 0)
            {
                Append("Could not send input.\n");
            }
            InputBox.Text = "";
        }

        async void Restart_Click(object sender, RoutedEventArgs e)
        {
            AppRestartFailureReason reason = await CoreApplication.RequestRestartAsync("");
            StatusText.Text = $"The app couldn't restart itself ({reason}). Close it and open it again.";
        }

        void Append(string text)
        {
            _output.Append(text);
            if (_output.Length > MaxOutputChars)
            {
                _output.Remove(0, _output.Length - MaxOutputChars);
            }
            OutputText.Text = _output.ToString();
            OutputScroll.UpdateLayout();
            OutputScroll.ChangeView(null, OutputScroll.ScrollableHeight, null, true);
        }

        static string LastError()
        {
            var sb = new StringBuilder(1024);
            Native.xb_last_error(sb, sb.Capacity);
            return sb.ToString();
        }

        static string Tail(string path, int maxBytes)
        {
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                long start = Math.Max(0, fs.Length - maxBytes);
                fs.Seek(start, SeekOrigin.Begin);
                using (var reader = new StreamReader(fs, Encoding.UTF8))
                {
                    return reader.ReadToEnd();
                }
            }
        }
    }
}
