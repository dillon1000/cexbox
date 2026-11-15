using System;
using System.Text;
using System.Threading.Tasks;
using Microsoft.Graphics.Canvas;
using Microsoft.Graphics.Canvas.Text;
using Microsoft.Graphics.Canvas.UI;
using Microsoft.Graphics.Canvas.UI.Xaml;
using Windows.System;
using Windows.UI;
using Windows.UI.Core;
using Windows.UI.Text;
using Windows.UI.Xaml;
using Windows.UI.Xaml.Controls;
using Windows.UI.Xaml.Input;

namespace Cexbox
{
    // Draws a VtTerminal with Win2D and turns keyboard and controller input into
    // the bytes a terminal would send.
    sealed class TerminalView : UserControl
    {
        const int MaxRun = 16;  // most characters drawn in one call
        const float FontSize = 10.5f;
        // Bundled with the app: the Xbox has no monospace font of its own.
        const string FontFamily = "ms-appx:///Fonts/CascadiaMono.ttf#Cascadia Mono";

        static readonly Color Background = Color.FromArgb(255, 20, 24, 22);
        static readonly Color Foreground = Color.FromArgb(255, 222, 228, 224);
        static readonly Color CursorColor = Color.FromArgb(255, 92, 201, 138);
        static readonly Color[] Palette = BuildPalette();

        CanvasControl _canvas;
        CanvasRenderTarget _frame;     // the terminal, drawn off-screen first
        bool _frameStale = true;
        CanvasTextFormat _regular, _bold;
        float _cellW = 8, _cellH = 16;
        bool _measured;
        char _pendingHigh;

        int _framesDrawn, _frameErrors, _rebuilds;
        DateTime _waitingSince = DateTime.MaxValue;  // when a redraw was requested and not yet drawn

        public VtTerminal Terminal { get; } = new VtTerminal(120, 40);

        // How many rows the view is scrolled back into the history.
        public int ScrollOffset { get; private set; }

        // Bytes to send to the program, as UTF-8 text.
        public event Action<string> Input;

        // The terminal's size in cells changed.
        public event Action<int, int> GridResized;

        // The controller's Y or View button was pressed.
        public event Action KeyboardRequested;
        public event Action LinkRequested;

        public string DrawStats => $"{_framesDrawn} frames drawn, {_frameErrors} failed, {_rebuilds} surface rebuilds";

        public TerminalView()
        {
            IsTabStop = true;
            UseSystemFocusVisuals = false;
            CreateCanvas();
            SizeChanged += (s, e) => UpdateGrid();
            PointerPressed += (s, e) => Focus(FocusState.Pointer);
            PointerWheelChanged += (s, e) =>
            {
                int delta = e.GetCurrentPoint(this).Properties.MouseWheelDelta;
                Scroll(delta > 0 ? 3 : -3);
                e.Handled = true;
            };
            Unloaded += (s, e) => _canvas.RemoveFromVisualTree();
        }

        void CreateCanvas()
        {
            if (_canvas != null)
            {
                _canvas.CreateResources -= OnCreateResources;
                _canvas.Draw -= OnDraw;
                _canvas.RemoveFromVisualTree();
            }
            _frame = null;
            _canvas = new CanvasControl { ClearColor = Background };
            _canvas.CreateResources += OnCreateResources;
            _canvas.Draw += OnDraw;
            Content = _canvas;
        }

        void RequestDraw()
        {
            _frameStale = true;
            if (_waitingSince == DateTime.MaxValue) _waitingSince = DateTime.UtcNow;
            _canvas.Invalidate();
        }

        // Called on every timer tick.
        public void Redraw()
        {
            if (Terminal.Dirty)
            {
                Terminal.Dirty = false;
                RequestDraw();
            }

            // If the surface was asked to draw and hasn't for two seconds, it has died
            // (an error inside the graphics stack can do that). Replace it.
            if (_waitingSince != DateTime.MaxValue && (DateTime.UtcNow - _waitingSince).TotalSeconds > 2 &&
                Window.Current.Visible && ActualWidth > 0 && _rebuilds < 30)
            {
                _rebuilds++;
                AppLog.Write($"the terminal surface stopped drawing; rebuilding it (time {_rebuilds})");
                CreateCanvas();
                _waitingSince = DateTime.UtcNow;
                _canvas.Invalidate();
            }
        }

        public void Scroll(int rows)
        {
            int max = Terminal.OnAltScreen ? 0 : Terminal.ScrollbackCount;
            int next = Math.Max(0, Math.Min(max, ScrollOffset + rows));
            if (next != ScrollOffset)
            {
                ScrollOffset = next;
                RequestDraw();
            }
        }

        // The screen as plain text, one line per row.
        public string ScreenText()
        {
            var sb = new StringBuilder();
            for (int y = 0; y < Terminal.Rows; y++)
            {
                Cell[] row = Terminal.ViewRow(y, ScrollOffset);
                sb.Append(row == null ? "" : VtTerminal.RowText(row)).Append("\r\n");
            }
            return sb.ToString();
        }

        // Saves the last frame drawn as a PNG. Returns null on success or what went wrong.
        public async Task<string> SaveScreenshotAsync(string path)
        {
            if (_frame == null) return "no frame has been drawn";
            try
            {
                await _frame.SaveAsync(path, CanvasBitmapFileFormat.Png);
                return null;
            }
            catch (Exception ex)
            {
                return ex.Message;
            }
        }

        void OnCreateResources(CanvasControl sender, CanvasCreateResourcesEventArgs args)
        {
            _frame = null;
            _regular = new CanvasTextFormat { FontFamily = FontFamily, FontSize = FontSize, WordWrapping = CanvasWordWrapping.NoWrap };
            _bold = new CanvasTextFormat { FontFamily = FontFamily, FontSize = FontSize, FontWeight = FontWeights.Bold, WordWrapping = CanvasWordWrapping.NoWrap };
            using (var layout = new CanvasTextLayout(sender, "MMMMMMMMMM", _regular, 10000, 10000))
            {
                _cellW = (float)layout.LayoutBounds.Width / 10f;
                _cellH = (float)Math.Ceiling(layout.LayoutBounds.Height);
            }
            _measured = true;
            if (!_probed)
            {
                _probed = true;
                ProbeStrokes(sender);
            }
            UpdateGrid();
        }

        bool _probed;

        void UpdateGrid()
        {
            if (!_measured || ActualWidth <= 0 || ActualHeight <= 0) return;
            int cols = Math.Max(20, (int)(ActualWidth / _cellW));
            int rows = Math.Max(5, (int)(ActualHeight / _cellH));
            if (cols == Terminal.Cols && rows == Terminal.Rows) return;
            Terminal.Resize(cols, rows);
            ScrollOffset = 0;
            GridResized?.Invoke(cols, rows);
            RequestDraw();
        }

        // The terminal is painted into an off-screen image inside a try block, and the
        // control only copies that image. Drawing errors surface when a drawing session
        // ends; ending the control's own session with an error would kill the control.
        void OnDraw(CanvasControl sender, CanvasDrawEventArgs args)
        {
            _waitingSince = DateTime.MaxValue;
            _framesDrawn++;
            if (!_measured) return;
            try
            {
                float w = (float)sender.ActualWidth, h = (float)sender.ActualHeight;
                if (w < 1 || h < 1) return;
                if (_frame == null || Math.Abs(_frame.Size.Width - w) > 0.5 || Math.Abs(_frame.Size.Height - h) > 0.5)
                {
                    _frame?.Dispose();
                    _frame = new CanvasRenderTarget(sender, w, h);
                    _frameStale = true;
                }
                if (_frameStale)
                {
                    _frameStale = false;
                    PaintFrame();
                }
            }
            catch (Exception ex)
            {
                if (++_frameErrors <= 10)
                {
                    AppLog.Write($"drawing a frame failed (0x{ex.HResult:X8}): {ex.Message}; characters on screen: {SpecialCharacters()}");
                }
            }

            // Show whatever was painted, even if part of the frame failed.
            try
            {
                if (_frame != null) args.DrawingSession.DrawImage(_frame);
            }
            catch (Exception ex)
            {
                _frame = null;
                if (++_frameErrors <= 10) AppLog.Write($"copying the frame to the screen failed (0x{ex.HResult:X8}): {ex.Message}");
            }
        }

        // After a frame fails, every drawing call is flushed on its own, so the log
        // names the call that fails and everything else still gets drawn.
        bool _careful;
        int _callErrors;

        void PaintFrame()
        {
            bool ok;
            using (CanvasDrawingSession ds = _frame.CreateDrawingSession())
            {
                ds.Clear(Background);
                Paint(ds);
                ok = Flush(ds, "the end of the frame");
            }
            if (ok || _careful) return;

            _frameErrors++;
            _careful = true;
            AppLog.Write("a frame failed to draw; drawing step by step from now on");
            using (CanvasDrawingSession ds = _frame.CreateDrawingSession())
            {
                ds.Clear(Background);
                Paint(ds);
                Flush(ds, "the end of the step-by-step frame");
            }
        }

        // Runs the drawing calls made so far. Returns false, and logs where, if one failed.
        bool Flush(CanvasDrawingSession ds, string where)
        {
            try
            {
                ds.Flush();
                return true;
            }
            catch (Exception ex)
            {
                if (++_callErrors <= 60) AppLog.Write($"drawing failed (0x{ex.HResult:X8}) at {where}");
                return false;
            }
        }

        // Checks one drawing call when in step-by-step mode.
        bool Check(CanvasDrawingSession ds, string what) => !_careful || Flush(ds, what);

        // Underlines are thin filled rectangles. Only fills and text are used anywhere:
        // stroked shapes (DrawLine, DrawRectangle) are what failed on Xbox.
        void Underline(CanvasDrawingSession ds, float x, float top, float width, Color color)
        {
            ds.FillRectangle(x, top + _cellH - 2, width, 1, color);
        }

        // Logs once whether the Xbox can draw a stroked line, to confirm the cause above.
        void ProbeStrokes(CanvasControl sender)
        {
            try
            {
                using (var probe = new CanvasRenderTarget(sender, 16, 16))
                using (CanvasDrawingSession ds = probe.CreateDrawingSession())
                {
                    ds.DrawLine(1, 1, 14, 14, Foreground);
                    ds.Flush();
                }
                AppLog.Write("stroke probe: drawing a line works");
            }
            catch (Exception ex)
            {
                AppLog.Write($"stroke probe: drawing a line fails (0x{ex.HResult:X8}): {ex.Message}");
            }
        }

        void DrawRun(CanvasDrawingSession ds, string s, float x, float y, Color color, CanvasTextFormat format)
        {
            ds.DrawText(s, x, y, color, format);
            if (Check(ds, $"text \"{s}\"") || s.Length < 2) return;

            // The run failed as a whole: draw it one character at a time instead.
            for (int i = 0; i < s.Length; i++)
            {
                ds.DrawText(s[i].ToString(), x + i * _cellW, y, color, format);
                Check(ds, $"character '{s[i]}'");
            }
        }

        // The distinct non-ASCII characters on screen, for the log.
        string SpecialCharacters()
        {
            var seen = new System.Collections.Generic.SortedSet<int>();
            for (int y = 0; y < Terminal.Rows; y++)
            {
                Cell[] row = Terminal.ViewRow(y, ScrollOffset);
                if (row == null) continue;
                foreach (Cell c in row)
                {
                    if (c.Ch >= 128 && seen.Count < 60) seen.Add(c.Ch);
                }
            }
            var sb = new StringBuilder();
            foreach (int cp in seen) sb.Append($"U+{cp:X4} ");
            return sb.ToString();
        }

        void Paint(CanvasDrawingSession ds)
        {
            VtTerminal t = Terminal;
            var text = new StringBuilder();

            for (int y = 0; y < t.Rows; y++)
            {
                Cell[] row = t.ViewRow(y, ScrollOffset);
                if (row == null) continue;
                float top = y * _cellH;
                int cols = Math.Min(row.Length, t.Cols);

                // Backgrounds, merged into runs of the same color.
                for (int x = 0; x < cols;)
                {
                    Color bg = BackColor(row[x]);
                    int end = x + 1;
                    while (end < cols && BackColor(row[end]) == bg) end++;
                    if (bg != Background)
                    {
                        ds.FillRectangle(x * _cellW, top, (end - x) * _cellW, _cellH, bg);
                        Check(ds, $"a background on row {y}");
                    }
                    x = end;
                }

                // Text. Plain ASCII runs are drawn together; anything else is drawn one
                // character at a time at its own cell, and only with characters the
                // bundled font has, so Windows never needs a fallback font.
                for (int x = 0; x < cols;)
                {
                    Cell c = row[x];
                    if ((c.Attr & CellAttr.WideTail) != 0 || c.Ch == 0 || c.Ch == ' ')
                    {
                        x++;
                        continue;
                    }
                    Color fg = ForeColor(c);
                    bool bold = (c.Attr & CellAttr.Bold) != 0;
                    if (c.Ch < 127)
                    {
                        text.Clear();
                        int end = x;
                        // Runs are kept short: long unbroken text (a link, a file path) has
                        // made drawing fail on Xbox.
                        while (end < cols && end - x < MaxRun && row[end].Ch > 32 && row[end].Ch < 127 &&
                               ForeColor(row[end]) == fg && ((row[end].Attr & CellAttr.Bold) != 0) == bold)
                        {
                            text.Append((char)row[end].Ch);
                            end++;
                        }
                        DrawRun(ds, text.ToString(), x * _cellW, top, fg, bold ? _bold : _regular);
                        if ((c.Attr & CellAttr.Underline) != 0)
                        {
                            Underline(ds, x * _cellW, top, (end - x) * _cellW, fg);
                            Check(ds, $"an underline on row {y}");
                        }
                        x = end;
                    }
                    else
                    {
                        int shown = FontCoverage.Resolve(c.Ch);
                        ds.DrawText(char.ConvertFromUtf32(shown), x * _cellW, top, fg, bold ? _bold : _regular);
                        Check(ds, $"character U+{shown:X4}");
                        if ((c.Attr & CellAttr.Underline) != 0)
                        {
                            Underline(ds, x * _cellW, top, _cellW, fg);
                            Check(ds, $"an underline on row {y}");
                        }
                        x++;
                    }
                }
            }

            if (t.CursorVisible && ScrollOffset == 0)
            {
                float cx = t.CursorX * _cellW, cy = t.CursorY * _cellH;
                byte alpha = FocusState != FocusState.Unfocused ? (byte)160 : (byte)80;
                ds.FillRectangle(cx, cy, _cellW, _cellH, Color.FromArgb(alpha, CursorColor.R, CursorColor.G, CursorColor.B));
                Check(ds, "the cursor");
            }
        }

        static Color ForeColor(Cell c)
        {
            bool inverse = (c.Attr & CellAttr.Inverse) != 0;
            Color color = inverse ? Resolve(c.Bg, Background) : Resolve(c.Fg, Foreground);
            if ((c.Attr & CellAttr.Dim) != 0) color = Color.FromArgb(255, (byte)(color.R * 0.6), (byte)(color.G * 0.6), (byte)(color.B * 0.6));
            return color;
        }

        static Color BackColor(Cell c)
        {
            bool inverse = (c.Attr & CellAttr.Inverse) != 0;
            return inverse ? Resolve(c.Fg, Foreground) : Resolve(c.Bg, Background);
        }

        static Color Resolve(int value, Color fallback)
        {
            if (value < 0) return fallback;
            if (value >= 0x1000000) return Color.FromArgb(255, (byte)(value >> 16), (byte)(value >> 8), (byte)value);
            return Palette[value & 0xff];
        }

        static Color[] BuildPalette()
        {
            var p = new Color[256];
            int[] basic =
            {
                0x1c2420, 0xe06c75, 0x5cc98a, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xc8d0cb,
                0x5c6a62, 0xff7b86, 0x7ee2a8, 0xffd68a, 0x7cc1ff, 0xd99cf0, 0x73d0db, 0xffffff,
            };
            for (int i = 0; i < 16; i++) p[i] = Color.FromArgb(255, (byte)(basic[i] >> 16), (byte)(basic[i] >> 8), (byte)basic[i]);
            int[] steps = { 0, 95, 135, 175, 215, 255 };
            for (int i = 0; i < 216; i++) p[16 + i] = Color.FromArgb(255, (byte)steps[i / 36], (byte)steps[i / 6 % 6], (byte)steps[i % 6]);
            for (int i = 0; i < 24; i++)
            {
                byte v = (byte)(8 + i * 10);
                p[232 + i] = Color.FromArgb(255, v, v, v);
            }
            return p;
        }

        static bool IsDown(VirtualKey key) =>
            (CoreWindow.GetForCurrentThread().GetKeyState(key) & CoreVirtualKeyStates.Down) == CoreVirtualKeyStates.Down;

        void Send(string s)
        {
            if (ScrollOffset != 0)
            {
                ScrollOffset = 0;
                RequestDraw();
            }
            AppLog.Write($"terminal sends \"{AppLog.Visible(s)}\"");
            Input?.Invoke(s);
        }

        string Arrow(char final) => (Terminal.AppCursorKeys ? "\x1bO" : "\x1b[") + final;

        // Turns a key or controller button into terminal input. Called by the page for
        // every key while the program runs, so it works whatever has focus. Returns
        // true if the key was used.
        public bool HandleKey(VirtualKey key)
        {
            bool ctrl = IsDown(VirtualKey.Control);
            bool shift = IsDown(VirtualKey.Shift);
            string s = null;
            switch (key)
            {
                case VirtualKey.Enter:
                case VirtualKey.GamepadA: s = "\r"; break;
                case VirtualKey.Back: s = "\b"; break;
                case VirtualKey.Tab: s = shift ? "\x1b[Z" : "\t"; break;
                case VirtualKey.Escape:
                case VirtualKey.GamepadB: s = "\x1b"; break;
                case VirtualKey.Up:
                case VirtualKey.GamepadDPadUp:
                case VirtualKey.GamepadLeftThumbstickUp: s = Arrow('A'); break;
                case VirtualKey.Down:
                case VirtualKey.GamepadDPadDown:
                case VirtualKey.GamepadLeftThumbstickDown: s = Arrow('B'); break;
                case VirtualKey.Right:
                case VirtualKey.GamepadDPadRight:
                case VirtualKey.GamepadLeftThumbstickRight: s = Arrow('C'); break;
                case VirtualKey.Left:
                case VirtualKey.GamepadDPadLeft:
                case VirtualKey.GamepadLeftThumbstickLeft: s = Arrow('D'); break;
                case VirtualKey.Home: s = "\x1b[H"; break;
                case VirtualKey.End: s = "\x1b[F"; break;
                case VirtualKey.Insert: s = "\x1b[2~"; break;
                case VirtualKey.Delete: s = "\x1b[3~"; break;
                case VirtualKey.PageUp:
                    if (shift) { Scroll(Terminal.Rows / 2); return true; }
                    s = "\x1b[5~";
                    break;
                case VirtualKey.PageDown:
                    if (shift) { Scroll(-Terminal.Rows / 2); return true; }
                    s = "\x1b[6~";
                    break;
                case VirtualKey.GamepadLeftShoulder: Scroll(Terminal.Rows / 2); return true;
                case VirtualKey.GamepadRightShoulder: Scroll(-Terminal.Rows / 2); return true;
                case VirtualKey.GamepadY: KeyboardRequested?.Invoke(); return true;
                case VirtualKey.GamepadView: LinkRequested?.Invoke(); return true;
                case VirtualKey.GamepadMenu: s = "\x03"; break;
                case VirtualKey.F1: s = "\x1bOP"; break;
                case VirtualKey.F2: s = "\x1bOQ"; break;
                case VirtualKey.F3: s = "\x1bOR"; break;
                case VirtualKey.F4: s = "\x1bOS"; break;
                case VirtualKey.F5: s = "\x1b[15~"; break;
                case VirtualKey.F6: s = "\x1b[17~"; break;
                case VirtualKey.F7: s = "\x1b[18~"; break;
                case VirtualKey.F8: s = "\x1b[19~"; break;
                case VirtualKey.F9: s = "\x1b[20~"; break;
                case VirtualKey.F10: s = "\x1b[21~"; break;
                case VirtualKey.F11: s = "\x1b[23~"; break;
                case VirtualKey.F12: s = "\x1b[24~"; break;
                default:
                    if (ctrl && key >= VirtualKey.A && key <= VirtualKey.Z) s = ((char)(key - VirtualKey.A + 1)).ToString();
                    else if (ctrl && key == VirtualKey.Space) s = "\0";
                    break;
            }
            if (s == null) return false;
            Send(s);
            return true;
        }

        // Turns a typed character into terminal input. Control characters come through
        // HandleKey instead. Returns true if the character was used.
        public bool HandleCharacter(char c, bool alt)
        {
            if (c < 0x20 || c == 0x7f) return false;
            if (char.IsHighSurrogate(c))
            {
                _pendingHigh = c;
                return true;
            }
            string s = c.ToString();
            if (char.IsLowSurrogate(c) && _pendingHigh != 0) s = new string(new[] { _pendingHigh, c });
            _pendingHigh = (char)0;
            if (alt) s = "\x1b" + s;
            Send(s);
            return true;
        }
    }
}
