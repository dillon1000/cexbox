using System;
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
        const float FontSize = 12f;
        // Bundled with the app: the Xbox has no monospace font of its own.
        const string FontFamily = "ms-appx:///Fonts/CascadiaMono.ttf#Cascadia Mono";

        static readonly Color Background = Color.FromArgb(255, 20, 24, 22);
        static readonly Color Foreground = Color.FromArgb(255, 222, 228, 224);
        static readonly Color CursorColor = Color.FromArgb(255, 92, 201, 138);
        static readonly Color[] Palette = BuildPalette();

        readonly CanvasControl _canvas = new CanvasControl();
        CanvasTextFormat _regular, _bold;
        float _cellW = 8, _cellH = 16;
        bool _measured;
        char _pendingHigh;

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

        public TerminalView()
        {
            IsTabStop = true;
            UseSystemFocusVisuals = false;
            Content = _canvas;
            _canvas.ClearColor = Background;
            _canvas.CreateResources += OnCreateResources;
            _canvas.Draw += OnDraw;
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

        public void Redraw()
        {
            if (Terminal.Dirty)
            {
                Terminal.Dirty = false;
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
                _canvas.Invalidate();
            }
        }

        void OnCreateResources(CanvasControl sender, CanvasCreateResourcesEventArgs args)
        {
            _regular = new CanvasTextFormat { FontFamily = FontFamily, FontSize = FontSize, WordWrapping = CanvasWordWrapping.NoWrap };
            _bold = new CanvasTextFormat { FontFamily = FontFamily, FontSize = FontSize, FontWeight = FontWeights.Bold, WordWrapping = CanvasWordWrapping.NoWrap };
            using (var layout = new CanvasTextLayout(sender, "MMMMMMMMMM", _regular, 10000, 10000))
            {
                _cellW = (float)layout.LayoutBounds.Width / 10f;
                _cellH = (float)Math.Ceiling(layout.LayoutBounds.Height);
            }
            _measured = true;
            UpdateGrid();
        }

        void UpdateGrid()
        {
            if (!_measured || ActualWidth <= 0 || ActualHeight <= 0) return;
            int cols = Math.Max(20, (int)(ActualWidth / _cellW));
            int rows = Math.Max(5, (int)(ActualHeight / _cellH));
            if (cols == Terminal.Cols && rows == Terminal.Rows) return;
            Terminal.Resize(cols, rows);
            ScrollOffset = 0;
            GridResized?.Invoke(cols, rows);
            _canvas.Invalidate();
        }

        void OnDraw(CanvasControl sender, CanvasDrawEventArgs args)
        {
            if (!_measured) return;
            var ds = args.DrawingSession;
            VtTerminal t = Terminal;
            var text = new System.Text.StringBuilder();

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
                    if (bg != Background) ds.FillRectangle(x * _cellW, top, (end - x) * _cellW, _cellH, bg);
                    x = end;
                }

                // Text. Plain ASCII runs are drawn together; anything else is drawn one
                // character at a time at its own cell, so fallback fonts can't shift the grid.
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
                    if (c.Ch < 128)
                    {
                        text.Clear();
                        int end = x;
                        while (end < cols && row[end].Ch > 32 && row[end].Ch < 128 &&
                               ForeColor(row[end]) == fg && ((row[end].Attr & CellAttr.Bold) != 0) == bold)
                        {
                            text.Append((char)row[end].Ch);
                            end++;
                        }
                        ds.DrawText(text.ToString(), x * _cellW, top, fg, bold ? _bold : _regular);
                        if ((c.Attr & CellAttr.Underline) != 0) ds.DrawLine(x * _cellW, top + _cellH - 1, end * _cellW, top + _cellH - 1, fg);
                        x = end;
                    }
                    else
                    {
                        ds.DrawText(char.ConvertFromUtf32(c.Ch), x * _cellW, top, fg, bold ? _bold : _regular);
                        if ((c.Attr & CellAttr.Underline) != 0) ds.DrawLine(x * _cellW, top + _cellH - 1, (x + 1) * _cellW, top + _cellH - 1, fg);
                        x++;
                    }
                }
            }

            if (t.CursorVisible && ScrollOffset == 0)
            {
                float cx = t.CursorX * _cellW, cy = t.CursorY * _cellH;
                if (FocusState != FocusState.Unfocused) ds.FillRectangle(cx, cy, _cellW, _cellH, Color.FromArgb(160, CursorColor.R, CursorColor.G, CursorColor.B));
                else ds.DrawRectangle(cx, cy, _cellW, _cellH, CursorColor);
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
                _canvas.Invalidate();
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
