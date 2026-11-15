using System;
using System.IO;
using System.Runtime.InteropServices.WindowsRuntime;
using System.Text;
using System.Threading.Tasks;
using QRCoder;
using Windows.ApplicationModel.Core;
using Windows.Storage;
using Windows.Storage.Streams;
using Windows.System;
using Windows.UI.Core;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;
using Windows.UI.Xaml.Media.Imaging;

namespace Cexbox
{
    public sealed partial class MainPage : Page
    {
        readonly string _root = ApplicationData.Current.LocalFolder.Path;
        readonly Decoder _utf8 = new UTF8Encoding(false).GetDecoder();
        readonly byte[] _readBuffer = new byte[65536];
        readonly char[] _charBuffer = new char[65536 * 2];
        DispatcherTimer _timer;
        bool _running;

        string ClaudeDir => Path.Combine(_root, "claude");
        string ClaudeExe => Path.Combine(ClaudeDir, "claude.exe");
        string HomeDir => Path.Combine(_root, "home");
        string LogDir => Path.Combine(_root, "logs");

        public MainPage()
        {
            InitializeComponent();
            AppLog.Reset();
            Loaded += OnLoaded;

            // While Claude Code runs, keys and controller buttons go to the terminal no matter
            // what has focus, except while typing in a text box or with the QR code showing.
            // AcceleratorKeyActivated runs before XAML sees the key, so a handled D-pad press
            // never also moves focus between buttons.
            CoreWindow window = Window.Current.CoreWindow;
            window.Activated += (s, e) => AppLog.Write($"window activation: {e.WindowActivationState}");
            window.VisibilityChanged += (s, e) => AppLog.Write($"window visible: {e.Visible}");
            window.PointerPressed += (s, e) => AppLog.Write("window pointer pressed");
            window.Dispatcher.AcceleratorKeyActivated += (s, e) =>
            {
                if (e.EventType != CoreAcceleratorKeyEventType.KeyDown && e.EventType != CoreAcceleratorKeyEventType.SystemKeyDown) return;
                object focused = FocusManager.GetFocusedElement();
                AppLog.Write($"window KeyDown {e.VirtualKey}, focus on {focused?.GetType().Name ?? "nothing"}" +
                             (focused is FrameworkElement fe && !string.IsNullOrEmpty(fe.Name) ? $" ({fe.Name})" : ""));
                if (LinkOverlay.Visibility == Visibility.Visible &&
                    (e.VirtualKey == VirtualKey.GamepadB || e.VirtualKey == VirtualKey.GamepadView || e.VirtualKey == VirtualKey.Escape))
                {
                    CloseLink_Click(this, null);
                    e.Handled = true;
                    return;
                }
                if (focused is TextBox && (e.VirtualKey == VirtualKey.Escape || e.VirtualKey == VirtualKey.GamepadB))
                {
                    // Leave the text box and go back to the terminal.
                    Terminal.Focus(FocusState.Programmatic);
                    e.Handled = true;
                    return;
                }
                if (KeysGoToTerminal(focused) && Terminal.HandleKey(e.VirtualKey)) e.Handled = true;
            };
            window.CharacterReceived += (s, e) =>
            {
                if (KeysGoToTerminal(FocusManager.GetFocusedElement()) &&
                    Terminal.HandleCharacter((char)e.KeyCode, e.KeyStatus.IsMenuKeyDown))
                {
                    e.Handled = true;
                }
            };

            Terminal.Input += s => SendToProgram(s);
            Terminal.Terminal.Reply = s => SendToProgram(s);
            Terminal.GridResized += (cols, rows) => Native.xb_console_resize(cols, rows);
            Terminal.KeyboardRequested += () => InputBox.Focus(FocusState.Keyboard);
            Terminal.LinkRequested += () => ShowLink();

            // The controller's B button means "back" on Xbox, which would close the app.
            SystemNavigationManager.GetForCurrentView().BackRequested += (s, e) => e.Handled = true;
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
                Print("\x1b[2mLoader log from the last run:\x1b[0m\r\n" + Tail(loaderLog, 6000).Replace("\r\n", "\n").Replace("\n", "\r\n") + "\r\n");
            }
            if (File.Exists(outputLog) && new FileInfo(outputLog).Length > 0)
            {
                Print("\x1b[2mProgram output from the last run:\x1b[0m\r\n" + Tail(outputLog, 4000) + "\x1b[0m\r\n");
            }

            _timer = new DispatcherTimer { Interval = TimeSpan.FromMilliseconds(16) };
            _timer.Tick += (s, args) =>
            {
                try
                {
                    Pump();
                }
                catch (Exception ex)
                {
                    if (++_pumpErrors <= 5) AppLog.Write("output pump failed: " + ex);
                }
            };
            _timer.Start();
            Terminal.Focus(FocusState.Programmatic);

            // For testing from a PC: LocalState\autostart.txt (uploaded with the Device Portal)
            // starts Claude Code right away, with the file's contents as arguments. It's
            // deleted first, so it only applies to one launch.
            string autostart = Path.Combine(_root, "autostart.txt");
            if (File.Exists(autostart) && File.Exists(ClaudeExe))
            {
                ArgsBox.Text = File.ReadAllText(autostart).Trim();
                File.Delete(autostart);
                var _ = Dispatcher.RunAsync(CoreDispatcherPriority.Low, () => Run_Click(this, null));
            }
        }

        bool KeysGoToTerminal(object focused) =>
            _running && LinkOverlay.Visibility == Visibility.Collapsed && !(focused is TextBox);

        void RefreshStatus()
        {
            if (File.Exists(ClaudeExe))
            {
                string versionFile = Path.Combine(ClaudeDir, "version.txt");
                string version = File.Exists(versionFile) ? File.ReadAllText(versionFile).Trim() : "(version unknown)";
                StatusText.Text = $"Claude Code {version} is ready.";
                RunButton.IsEnabled = !_running;
            }
            else
            {
                StatusText.Text = "Download Claude Code to start. You can also copy claude.exe into LocalState\\claude with the Device Portal.";
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
                await ClaudeDownloader.DownloadAsync(ClaudeDir, (done, total) =>
                {
                    var _ = Dispatcher.RunAsync(CoreDispatcherPriority.Low, () =>
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
                RunButton.IsEnabled = File.Exists(ClaudeExe) && !_running;
            }
            finally
            {
                Progress.Visibility = Visibility.Collapsed;
                DownloadButton.IsEnabled = !_running;
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
            // There's no PowerShell on Xbox, so Claude Code shouldn't offer that tool.
            Native.xb_setenv("CLAUDE_CODE_USE_POWERSHELL_TOOL", "0");

            // Claude Code needs a bash for its Bash tool; busybox provides one.
            string bin = Path.Combine(_root, "bin");
            string bash = Path.Combine(bin, "bash.exe");
            if (!File.Exists(bash))
            {
                StatusText.Text = "Downloading the shell Claude Code uses (busybox)…";
                try
                {
                    await ClaudeDownloader.DownloadBusyboxAsync(bin);
                }
                catch (Exception ex)
                {
                    Print("Could not download busybox: " + ex.Message + "\r\n");
                }
            }
            if (File.Exists(bash))
            {
                Native.xb_setenv("CLAUDE_CODE_GIT_BASH_PATH", bash);
                Native.xb_setenv("PATH", bin + ";" + (Environment.GetEnvironmentVariable("PATH") ?? ""));
            }
            Native.xb_setenv("TERM", "xterm-256color");
            Native.xb_setenv("COLORTERM", "truecolor");
            // Makes links come through as OSC 8 hyperlinks, which the sign-in button picks up.
            Native.xb_setenv("FORCE_HYPERLINK", "1");

            string args = ArgsBox.Text.Trim();
            string commandLine = "\"" + ClaudeExe + "\"" + (args.Length > 0 ? " " + args : "");
            Native.xb_console_resize(Terminal.Terminal.Cols, Terminal.Terminal.Rows);
            Print("\x1b[2J\x1b[3J\x1b[H");

            _running = true;
            RunButton.IsEnabled = false;
            DownloadButton.IsEnabled = false;
            RestartButton.Visibility = Visibility.Visible;
            StatusText.Text = "Loading claude.exe…";

            // Loading reads the whole 370 MB file, so keep it off the UI thread.
            string exe = ClaudeExe, home = HomeDir, logs = LogDir;
            int rc = await Task.Run(() => Native.xb_start(exe, commandLine, home, logs, Native.FlagConsole));
            if (rc != 0)
            {
                Print("Could not start: " + LastError() + "\r\n");
                StatusText.Text = "Claude Code didn't start. The loader log in LocalState\\logs has details.";
                return;
            }
            StatusText.Text = "Claude Code is running. Restart the app to start it again after it exits.";
            Terminal.Focus(FocusState.Programmatic);
        }

        int _pumps;
        int _pumpErrors;

        // For testing from a PC: LocalState\input.txt (uploaded with the Device Portal) is
        // sent to the program as keystrokes, then deleted. \e, \r, \n, \t and \xNN are escapes.
        void CheckRemoteInput()
        {
            string path = Path.Combine(_root, "input.txt");
            if (!File.Exists(path)) return;
            string raw;
            try
            {
                raw = File.ReadAllText(path);
                File.Delete(path);
            }
            catch (IOException)
            {
                return;
            }
            var sb = new StringBuilder();
            for (int i = 0; i < raw.Length; i++)
            {
                if (raw[i] != '\\' || i + 1 >= raw.Length)
                {
                    sb.Append(raw[i]);
                    continue;
                }
                char c = raw[++i];
                if (c == 'e') sb.Append('\x1b');
                else if (c == 'r') sb.Append('\r');
                else if (c == 'n') sb.Append('\n');
                else if (c == 't') sb.Append('\t');
                else if (c == 'x' && i + 2 < raw.Length)
                {
                    sb.Append((char)Convert.ToInt32(raw.Substring(i + 1, 2), 16));
                    i += 2;
                }
                else sb.Append(c);
            }
            AppLog.Write($"remote input \"{AppLog.Visible(sb.ToString())}\"");
            SendToProgram(sb.ToString());
        }

        // For testing from a PC: when LocalState\screenshot.txt appears, the terminal's
        // text and last drawn frame are saved to LocalState\logs.
        async void CheckScreenshotRequest()
        {
            string request = Path.Combine(_root, "screenshot.txt");
            if (!File.Exists(request)) return;
            try
            {
                File.Delete(request);
                File.WriteAllText(Path.Combine(LogDir, "screen.txt"), Terminal.ScreenText());
                string problem = await Terminal.SaveScreenshotAsync(Path.Combine(LogDir, "screen.png"));
                AppLog.Write(problem == null ? "screenshot saved" : "screenshot failed: " + problem);
            }
            catch (IOException ex)
            {
                AppLog.Write("screenshot failed: " + ex.Message);
            }
        }

        void Pump()
        {
            if (++_pumps % 30 == 0)
            {
                CheckRemoteInput();
                CheckScreenshotRequest();
            }
            if (_pumps % 300 == 0)
            {
                CoreWindow w = Window.Current.CoreWindow;
                AppLog.Write($"window state: activation {w.ActivationMode}, visible {w.Visible}, " +
                             $"focus on {FocusManager.GetFocusedElement()?.GetType().Name ?? "nothing"}; {Terminal.DrawStats}");
            }
            if (_running)
            {
                int n;
                while ((n = Native.xb_read_output(_readBuffer, _readBuffer.Length)) > 0)
                {
                    int chars = _utf8.GetChars(_readBuffer, 0, n, _charBuffer, 0, false);
                    Terminal.Terminal.Feed(new string(_charBuffer, 0, chars));
                }

                int state = Native.xb_poll(out int exitCode);
                if (state == Native.Exited || state == Native.Failed)
                {
                    _running = false;
                    Print(state == Native.Exited
                        ? $"\r\n\x1b[2mClaude Code exited with code {exitCode}.\x1b[0m\r\n"
                        : "\r\n\x1b[2mFailed: " + LastError() + "\x1b[0m\r\n");
                    StatusText.Text = "Finished. Restart the app to start Claude Code again.";
                }
            }
            Terminal.Redraw();
        }

        void Print(string text) => Terminal.Terminal.Feed(text);

        void SendToProgram(string text)
        {
            if (!_running)
            {
                AppLog.Write("input dropped: Claude Code isn't running");
                return;
            }
            byte[] bytes = Encoding.UTF8.GetBytes(text);
            Native.xb_console_input(bytes, bytes.Length);
        }

        void SendInputBox()
        {
            string text = InputBox.Text;
            if (text.Length > 0)
            {
                // Sent as a paste, so a long code or prompt arrives as one piece.
                SendToProgram(Terminal.Terminal.BracketedPaste ? "\x1b[200~" + text + "\x1b[201~" : text);
            }
            SendToProgram("\r");
            InputBox.Text = "";
        }

        void Send_Click(object sender, RoutedEventArgs e) => SendInputBox();

        void InputBox_KeyDown(object sender, KeyRoutedEventArgs e)
        {
            if (e.Key == VirtualKey.Enter)
            {
                SendInputBox();
                e.Handled = true;
            }
        }

        void Focus_Click(object sender, RoutedEventArgs e) => Terminal.Focus(FocusState.Programmatic);

        void Link_Click(object sender, RoutedEventArgs e) => ShowLink();

        async void ShowLink()
        {
            string link = Terminal.Terminal.LastLink ?? Terminal.Terminal.FindLinkOnScreen();
            if (string.IsNullOrEmpty(link))
            {
                StatusText.Text = "No link on screen yet. Run /login in Claude Code first.";
                return;
            }
            var generator = new QRCodeGenerator();
            QRCodeData data = generator.CreateQrCode(link, QRCodeGenerator.ECCLevel.L);
            byte[] png = new PngByteQRCode(data).GetGraphic(10);
            var image = new BitmapImage();
            using (var stream = new InMemoryRandomAccessStream())
            {
                await stream.WriteAsync(png.AsBuffer());
                stream.Seek(0);
                await image.SetSourceAsync(stream);
            }
            LinkImage.Source = image;
            LinkText.Text = link;
            LinkOverlay.Visibility = Visibility.Visible;
            CloseLinkButton.Focus(FocusState.Programmatic);
        }

        void CloseLink_Click(object sender, RoutedEventArgs e)
        {
            LinkOverlay.Visibility = Visibility.Collapsed;
            Terminal.Focus(FocusState.Programmatic);
        }

        async void Restart_Click(object sender, RoutedEventArgs e)
        {
            AppRestartFailureReason reason = await CoreApplication.RequestRestartAsync("");
            StatusText.Text = $"The app couldn't restart itself ({reason}). Close it and open it again.";
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
