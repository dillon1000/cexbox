using System;
using System.Collections.Generic;
using System.Text;

namespace Cexbox
{
    // One character cell on screen.
    struct Cell
    {
        public int Ch;      // Unicode code point; 0 means blank
        public int Fg;      // -1 default, 0-255 palette, 0x1000000 | rgb for true color
        public int Bg;
        public byte Attr;
    }

    static class CellAttr
    {
        public const byte Bold = 1;
        public const byte Dim = 2;
        public const byte Italic = 4;
        public const byte Underline = 8;
        public const byte Inverse = 16;
        public const byte Wide = 32;      // first half of a double-width character
        public const byte WideTail = 64;  // second half, drawn by the first
    }

    // An xterm-compatible terminal screen: parses the program's output (text plus
    // escape sequences) into a grid of cells, with scrollback and an alternate screen.
    // Covers what Claude Code's interface (Ink) and common command-line tools use.
    sealed class VtTerminal
    {
        const int MaxScrollback = 3000;

        enum State { Ground, Escape, EscapeSkip, Csi, Osc, OscEscape, Dcs, DcsEscape }

        Cell[][] _main;
        Cell[][] _alt;
        Cell[][] _screen;
        readonly List<Cell[]> _scrollback = new List<Cell[]>();

        State _state = State.Ground;
        readonly List<int> _params = new List<int>();
        int _param = -1;
        char _private;
        readonly StringBuilder _intermediate = new StringBuilder();
        readonly StringBuilder _osc = new StringBuilder();
        char _pendingHigh;

        int _fg = -1, _bg = -1;
        byte _attr;
        bool _wrapPending;
        int _top, _bottom;
        int _savedX, _savedY, _savedFg = -1, _savedBg = -1;
        byte _savedAttr;

        public int Cols { get; private set; }
        public int Rows { get; private set; }
        public int CursorX { get; private set; }
        public int CursorY { get; private set; }
        public bool CursorVisible { get; private set; } = true;
        public bool AppCursorKeys { get; private set; }
        public bool BracketedPaste { get; private set; }
        public bool AutoWrap { get; private set; } = true;
        public bool OnAltScreen => _screen == _alt;
        public int ScrollbackCount => _scrollback.Count;
        public bool Dirty { get; set; } = true;

        // The most recent link the program marked with an OSC 8 hyperlink.
        public string LastLink { get; private set; }

        // Called with replies to queries such as "where is the cursor".
        public Action<string> Reply;

        public VtTerminal(int cols, int rows)
        {
            Cols = cols;
            Rows = rows;
            _main = NewScreen(cols, rows);
            _alt = NewScreen(cols, rows);
            _screen = _main;
            _top = 0;
            _bottom = rows - 1;
        }

        static Cell[][] NewScreen(int cols, int rows)
        {
            var s = new Cell[rows][];
            for (int y = 0; y < rows; y++) s[y] = NewRow(cols, -1);
            return s;
        }

        static Cell[] NewRow(int cols, int bg)
        {
            var r = new Cell[cols];
            for (int x = 0; x < cols; x++) r[x] = new Cell { Ch = 0, Fg = -1, Bg = bg };
            return r;
        }

        Cell Blank => new Cell { Ch = 0, Fg = -1, Bg = _bg };

        // Returns the rows to draw, offset rows up into the scrollback.
        public Cell[] ViewRow(int y, int scrollOffset)
        {
            if (OnAltScreen || scrollOffset <= 0) return _screen[y];
            int index = _scrollback.Count - scrollOffset + y;
            if (index < 0) return null;
            if (index < _scrollback.Count) return _scrollback[index];
            return _screen[index - _scrollback.Count];
        }

        public void Feed(string text)
        {
            for (int i = 0; i < text.Length; i++)
            {
                char c = text[i];
                if (_pendingHigh != 0)
                {
                    char high = _pendingHigh;
                    _pendingHigh = (char)0;
                    if (char.IsLowSurrogate(c))
                    {
                        Step(char.ConvertToUtf32(high, c));
                        continue;
                    }
                }
                if (char.IsHighSurrogate(c))
                {
                    if (i + 1 < text.Length && char.IsLowSurrogate(text[i + 1]))
                    {
                        Step(char.ConvertToUtf32(c, text[i + 1]));
                        i++;
                    }
                    else
                    {
                        _pendingHigh = c;
                    }
                    continue;
                }
                Step(c);
            }
            Dirty = true;
        }

        void Step(int c)
        {
            switch (_state)
            {
                case State.Ground:
                    if (c == 0x1b) _state = State.Escape;
                    else if (c < 0x20) Control(c);
                    else if (c != 0x7f) Print(c);
                    break;

                case State.Escape:
                    EscapeChar(c);
                    break;

                case State.EscapeSkip:
                    _state = State.Ground;
                    break;

                case State.Csi:
                    if (c >= '0' && c <= '9')
                    {
                        _param = (_param < 0 ? 0 : _param) * 10 + (c - '0');
                        if (_param > 100000) _param = 100000;
                    }
                    else if (c == ';' || c == ':')
                    {
                        _params.Add(_param);
                        _param = -1;
                    }
                    else if ((c == '?' || c == '>' || c == '=' || c == '<') && _params.Count == 0 && _param < 0)
                    {
                        _private = (char)c;
                    }
                    else if (c >= 0x20 && c <= 0x2f)
                    {
                        _intermediate.Append((char)c);
                    }
                    else if (c >= 0x40 && c <= 0x7e)
                    {
                        if (_param >= 0 || _params.Count > 0) _params.Add(_param);
                        _state = State.Ground;
                        Csi((char)c);
                    }
                    else if (c == 0x1b)
                    {
                        _state = State.Escape;
                    }
                    else if (c < 0x20)
                    {
                        Control(c);
                    }
                    break;

                case State.Osc:
                    if (c == 0x07) { Osc(); _state = State.Ground; }
                    else if (c == 0x1b) _state = State.OscEscape;
                    else if (_osc.Length < 8192) _osc.Append(char.ConvertFromUtf32(c));
                    break;

                case State.OscEscape:
                    Osc();
                    _state = State.Ground;
                    if (c != '\\') Step(c);
                    break;

                case State.Dcs:
                    if (c == 0x1b) _state = State.DcsEscape;
                    break;

                case State.DcsEscape:
                    _state = c == '\\' ? State.Ground : State.Dcs;
                    break;
            }
        }

        void EscapeChar(int c)
        {
            _state = State.Ground;
            switch (c)
            {
                case '[':
                    _params.Clear();
                    _param = -1;
                    _private = '\0';
                    _intermediate.Clear();
                    _state = State.Csi;
                    break;
                case ']':
                    _osc.Clear();
                    _state = State.Osc;
                    break;
                case 'P':
                    _state = State.Dcs;
                    break;
                case '(':
                case ')':
                case '*':
                case '+':
                case '#':
                case '%':
                    _state = State.EscapeSkip;
                    break;
                case '7': SaveCursor(); break;
                case '8': RestoreCursor(); break;
                case 'D': Index(); break;
                case 'E': CursorX = 0; Index(); break;
                case 'M': ReverseIndex(); break;
                case 'c': Reset(); break;
            }
        }

        void Control(int c)
        {
            switch (c)
            {
                case 0x08:
                    if (CursorX > 0) CursorX--;
                    _wrapPending = false;
                    break;
                case 0x09:
                    CursorX = Math.Min(Cols - 1, (CursorX / 8 + 1) * 8);
                    _wrapPending = false;
                    break;
                case 0x0a:
                case 0x0b:
                case 0x0c:
                    // The Windows console returns to the first column on a line feed.
                    CursorX = 0;
                    Index();
                    break;
                case 0x0d:
                    CursorX = 0;
                    _wrapPending = false;
                    break;
            }
        }

        void Print(int c)
        {
            int width = CharWidth(c);
            if (width == 0) return;
            if (_wrapPending && AutoWrap)
            {
                CursorX = 0;
                Index();
            }
            _wrapPending = false;
            if (CursorX + width > Cols)
            {
                if (AutoWrap)
                {
                    CursorX = 0;
                    Index();
                }
                else
                {
                    CursorX = Math.Max(0, Cols - width);
                }
            }
            Cell[] row = _screen[CursorY];
            row[CursorX] = new Cell { Ch = c, Fg = _fg, Bg = _bg, Attr = (byte)(_attr | (width == 2 ? CellAttr.Wide : 0)) };
            if (width == 2 && CursorX + 1 < Cols)
            {
                row[CursorX + 1] = new Cell { Ch = 0, Fg = _fg, Bg = _bg, Attr = (byte)(_attr | CellAttr.WideTail) };
            }
            CursorX += width;
            if (CursorX >= Cols)
            {
                CursorX = Cols - 1;
                _wrapPending = true;
            }
        }

        void Index()
        {
            _wrapPending = false;
            if (CursorY == _bottom) ScrollUp(1);
            else if (CursorY < Rows - 1) CursorY++;
        }

        void ReverseIndex()
        {
            _wrapPending = false;
            if (CursorY == _top) ScrollDown(1);
            else if (CursorY > 0) CursorY--;
        }

        void ScrollUp(int n)
        {
            for (int i = 0; i < n; i++)
            {
                if (_top == 0 && !OnAltScreen)
                {
                    _scrollback.Add(_screen[0]);
                    if (_scrollback.Count > MaxScrollback) _scrollback.RemoveAt(0);
                }
                for (int y = _top; y < _bottom; y++) _screen[y] = _screen[y + 1];
                _screen[_bottom] = NewRow(Cols, _bg);
            }
        }

        void ScrollDown(int n)
        {
            for (int i = 0; i < n; i++)
            {
                for (int y = _bottom; y > _top; y--) _screen[y] = _screen[y - 1];
                _screen[_top] = NewRow(Cols, _bg);
            }
        }

        int Param(int i, int fallback)
        {
            if (i >= _params.Count) return fallback;
            int v = _params[i];
            return v <= 0 ? fallback : v;
        }

        int RawParam(int i) => i < _params.Count && _params[i] > 0 ? _params[i] : 0;

        void Csi(char final)
        {
            if (_private == '?')
            {
                if (final == 'h' || final == 'l') PrivateMode(final == 'h');
                return;
            }
            if (_private == '>')
            {
                if (final == 'c') Reply?.Invoke("\x1b[>0;10;1c");
                return;
            }
            if (_private != '\0' || _intermediate.Length > 0) return;

            int n = Param(0, 1);
            switch (final)
            {
                case 'A': CursorY = Math.Max(CursorY >= _top ? _top : 0, CursorY - n); _wrapPending = false; break;
                case 'B':
                case 'e': CursorY = Math.Min(CursorY <= _bottom ? _bottom : Rows - 1, CursorY + n); _wrapPending = false; break;
                case 'C':
                case 'a': CursorX = Math.Min(Cols - 1, CursorX + n); _wrapPending = false; break;
                case 'D': CursorX = Math.Max(0, CursorX - n); _wrapPending = false; break;
                case 'E': CursorX = 0; CursorY = Math.Min(Rows - 1, CursorY + n); _wrapPending = false; break;
                case 'F': CursorX = 0; CursorY = Math.Max(0, CursorY - n); _wrapPending = false; break;
                case 'G':
                case '`': CursorX = Clamp(n - 1, 0, Cols - 1); _wrapPending = false; break;
                case 'H':
                case 'f':
                    CursorY = Clamp(Param(0, 1) - 1, 0, Rows - 1);
                    CursorX = Clamp(Param(1, 1) - 1, 0, Cols - 1);
                    _wrapPending = false;
                    break;
                case 'd': CursorY = Clamp(n - 1, 0, Rows - 1); _wrapPending = false; break;
                case 'J': EraseDisplay(RawParam(0)); break;
                case 'K': EraseLine(RawParam(0)); break;
                case 'L': InsertLines(n); break;
                case 'M': DeleteLines(n); break;
                case 'P': DeleteChars(n); break;
                case '@': InsertChars(n); break;
                case 'X': EraseChars(n); break;
                case 'S': ScrollUp(n); break;
                case 'T': ScrollDown(n); break;
                case 'm': Sgr(); break;
                case 'r':
                    {
                        int top = Param(0, 1) - 1, bottom = Param(1, Rows) - 1;
                        if (top < bottom && bottom < Rows)
                        {
                            _top = top;
                            _bottom = bottom;
                        }
                        CursorX = 0;
                        CursorY = 0;
                        _wrapPending = false;
                        break;
                    }
                case 's': SaveCursor(); break;
                case 'u': RestoreCursor(); break;
                case 'n':
                    if (RawParam(0) == 6) Reply?.Invoke($"\x1b[{CursorY + 1};{CursorX + 1}R");
                    else if (RawParam(0) == 5) Reply?.Invoke("\x1b[0n");
                    break;
                case 'c': Reply?.Invoke("\x1b[?62;22c"); break;
                case 't':
                    if (RawParam(0) == 18) Reply?.Invoke($"\x1b[8;{Rows};{Cols}t");
                    break;
            }
        }

        void PrivateMode(bool on)
        {
            foreach (int p in _params)
            {
                switch (p)
                {
                    case 1: AppCursorKeys = on; break;
                    case 7: AutoWrap = on; break;
                    case 25: CursorVisible = on; break;
                    case 2004: BracketedPaste = on; break;
                    case 47:
                    case 1047:
                    case 1049:
                        if (on && !OnAltScreen)
                        {
                            if (p == 1049) SaveCursor();
                            _alt = NewScreen(Cols, Rows);
                            _screen = _alt;
                        }
                        else if (!on && OnAltScreen)
                        {
                            _screen = _main;
                            if (p == 1049) RestoreCursor();
                        }
                        _top = 0;
                        _bottom = Rows - 1;
                        break;
                }
            }
        }

        void Sgr()
        {
            if (_params.Count == 0)
            {
                ResetStyle();
                return;
            }
            for (int i = 0; i < _params.Count; i++)
            {
                int p = Math.Max(0, _params[i]);
                if (p == 0) ResetStyle();
                else if (p == 1) _attr |= CellAttr.Bold;
                else if (p == 2) _attr |= CellAttr.Dim;
                else if (p == 3) _attr |= CellAttr.Italic;
                else if (p == 4) _attr |= CellAttr.Underline;
                else if (p == 7) _attr |= CellAttr.Inverse;
                else if (p == 22) _attr &= unchecked((byte)~(CellAttr.Bold | CellAttr.Dim));
                else if (p == 23) _attr &= unchecked((byte)~CellAttr.Italic);
                else if (p == 24) _attr &= unchecked((byte)~CellAttr.Underline);
                else if (p == 27) _attr &= unchecked((byte)~CellAttr.Inverse);
                else if (p >= 30 && p <= 37) _fg = p - 30;
                else if (p == 39) _fg = -1;
                else if (p >= 40 && p <= 47) _bg = p - 40;
                else if (p == 49) _bg = -1;
                else if (p >= 90 && p <= 97) _fg = p - 90 + 8;
                else if (p >= 100 && p <= 107) _bg = p - 100 + 8;
                else if (p == 38 || p == 48)
                {
                    int color = -1;
                    int mode = i + 1 < _params.Count ? _params[i + 1] : -1;
                    if (mode == 5 && i + 2 < _params.Count)
                    {
                        color = Clamp(_params[i + 2], 0, 255);
                        i += 2;
                    }
                    else if (mode == 2 && i + 4 < _params.Count)
                    {
                        int r = Clamp(_params[i + 2], 0, 255), g = Clamp(_params[i + 3], 0, 255), b = Clamp(_params[i + 4], 0, 255);
                        color = 0x1000000 | (r << 16) | (g << 8) | b;
                        i += 4;
                    }
                    if (p == 38) _fg = color; else _bg = color;
                }
            }
        }

        void ResetStyle()
        {
            _fg = -1;
            _bg = -1;
            _attr = 0;
        }

        void Osc()
        {
            string s = _osc.ToString();
            // Programs ask for the text and background colors to pick a light or dark theme.
            if (s == "10;?") Reply?.Invoke("\x1b]10;rgb:dede/e4e4/e0e0\x1b\\");
            else if (s == "11;?") Reply?.Invoke("\x1b]11;rgb:1414/1818/1616\x1b\\");
            // OSC 8 ; params ; uri  marks a hyperlink.
            if (s.StartsWith("8;"))
            {
                int second = s.IndexOf(';', 2);
                if (second >= 0)
                {
                    string uri = s.Substring(second + 1);
                    if (uri.StartsWith("http://") || uri.StartsWith("https://")) LastLink = uri;
                }
            }
        }

        void EraseDisplay(int mode)
        {
            if (mode == 0)
            {
                EraseLine(0);
                for (int y = CursorY + 1; y < Rows; y++) _screen[y] = NewRow(Cols, _bg);
            }
            else if (mode == 1)
            {
                EraseLine(1);
                for (int y = 0; y < CursorY; y++) _screen[y] = NewRow(Cols, _bg);
            }
            else
            {
                for (int y = 0; y < Rows; y++) _screen[y] = NewRow(Cols, _bg);
                if (mode == 3) _scrollback.Clear();
            }
        }

        void EraseLine(int mode)
        {
            Cell[] row = _screen[CursorY];
            int from = mode == 0 ? CursorX : 0;
            int to = mode == 1 ? CursorX : Cols - 1;
            for (int x = from; x <= to && x < Cols; x++) row[x] = Blank;
            _wrapPending = false;
        }

        void InsertLines(int n)
        {
            if (CursorY < _top || CursorY > _bottom) return;
            int saveTop = _top;
            _top = CursorY;
            ScrollDown(Math.Min(n, _bottom - CursorY + 1));
            _top = saveTop;
            CursorX = 0;
        }

        void DeleteLines(int n)
        {
            if (CursorY < _top || CursorY > _bottom) return;
            int saveTop = _top;
            _top = CursorY;
            for (int i = 0; i < Math.Min(n, _bottom - CursorY + 1); i++)
            {
                for (int y = _top; y < _bottom; y++) _screen[y] = _screen[y + 1];
                _screen[_bottom] = NewRow(Cols, _bg);
            }
            _top = saveTop;
            CursorX = 0;
        }

        void DeleteChars(int n)
        {
            Cell[] row = _screen[CursorY];
            n = Math.Min(n, Cols - CursorX);
            for (int x = CursorX; x < Cols; x++) row[x] = x + n < Cols ? row[x + n] : Blank;
        }

        void InsertChars(int n)
        {
            Cell[] row = _screen[CursorY];
            n = Math.Min(n, Cols - CursorX);
            for (int x = Cols - 1; x >= CursorX; x--) row[x] = x - n >= CursorX ? row[x - n] : Blank;
        }

        void EraseChars(int n)
        {
            Cell[] row = _screen[CursorY];
            for (int x = CursorX; x < Math.Min(Cols, CursorX + n); x++) row[x] = Blank;
        }

        void SaveCursor()
        {
            _savedX = CursorX;
            _savedY = CursorY;
            _savedFg = _fg;
            _savedBg = _bg;
            _savedAttr = _attr;
        }

        void RestoreCursor()
        {
            CursorX = Clamp(_savedX, 0, Cols - 1);
            CursorY = Clamp(_savedY, 0, Rows - 1);
            _fg = _savedFg;
            _bg = _savedBg;
            _attr = _savedAttr;
            _wrapPending = false;
        }

        void Reset()
        {
            _main = NewScreen(Cols, Rows);
            _alt = NewScreen(Cols, Rows);
            _screen = _main;
            ResetStyle();
            CursorX = CursorY = 0;
            _top = 0;
            _bottom = Rows - 1;
            CursorVisible = true;
            AutoWrap = true;
            AppCursorKeys = false;
            BracketedPaste = false;
        }

        public void Resize(int cols, int rows)
        {
            if (cols == Cols && rows == Rows) return;
            // Keep the cursor's line on screen: lines that no longer fit above it go to scrollback.
            int shift = Math.Max(0, CursorY - (rows - 1));
            for (int i = 0; i < shift; i++)
            {
                _scrollback.Add(Fit(_main[i], cols));
                if (_scrollback.Count > MaxScrollback) _scrollback.RemoveAt(0);
            }
            var main = new Cell[rows][];
            for (int y = 0; y < rows; y++)
            {
                int from = y + shift;
                main[y] = from < Rows ? Fit(_main[from], cols) : NewRow(cols, -1);
            }
            bool wasAlt = OnAltScreen;
            _main = main;
            _alt = NewScreen(cols, rows);
            _screen = wasAlt ? _alt : _main;
            Cols = cols;
            Rows = rows;
            CursorY = Clamp(CursorY - shift, 0, rows - 1);
            CursorX = Clamp(CursorX, 0, cols - 1);
            _top = 0;
            _bottom = rows - 1;
            _wrapPending = false;
            Dirty = true;
        }

        static Cell[] Fit(Cell[] row, int cols)
        {
            var r = NewRow(cols, -1);
            Array.Copy(row, r, Math.Min(cols, row.Length));
            return r;
        }

        // Text of a row with trailing blanks removed.
        public static string RowText(Cell[] row)
        {
            var sb = new StringBuilder();
            foreach (Cell c in row)
            {
                if ((c.Attr & CellAttr.WideTail) != 0) continue;
                sb.Append(c.Ch == 0 ? " " : char.ConvertFromUtf32(c.Ch));
            }
            return sb.ToString().TrimEnd();
        }

        // Finds the last link on screen, joining it back together if the program
        // wrapped it across lines.
        public string FindLinkOnScreen()
        {
            var lines = new List<string>();
            int start = Math.Max(0, _scrollback.Count - 200);
            for (int i = start; i < _scrollback.Count; i++) lines.Add(RowText(_scrollback[i]));
            for (int y = 0; y < Rows; y++) lines.Add(RowText(_screen[y]));

            for (int i = lines.Count - 1; i >= 0; i--)
            {
                int at = lines[i].LastIndexOf("https://", StringComparison.Ordinal);
                if (at < 0) continue;
                var link = new StringBuilder(TakeLinkChars(lines[i].Substring(at)));
                bool reachedEdge = at + link.Length >= Cols - 2;
                for (int j = i + 1; j < lines.Count && reachedEdge; j++)
                {
                    string next = TakeLinkChars(lines[j].TrimStart());
                    if (next.Length == 0) break;
                    link.Append(next);
                    reachedEdge = lines[j].Length >= Cols - 2;
                }
                return link.ToString();
            }
            return null;
        }

        static string TakeLinkChars(string s)
        {
            int n = 0;
            while (n < s.Length && IsLinkChar(s[n])) n++;
            return s.Substring(0, n);
        }

        static bool IsLinkChar(char c) =>
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            "-._~:/?#[]@!$&'()*+,;=%".IndexOf(c) >= 0;

        static int Clamp(int v, int min, int max) => v < min ? min : (v > max ? max : v);

        // Display width in cells, close to what Ink assumes when it lays out text.
        public static int CharWidth(int c)
        {
            if (c == 0) return 0;
            if ((c >= 0x0300 && c <= 0x036f) || (c >= 0x200b && c <= 0x200f) || (c >= 0xfe00 && c <= 0xfe0f) ||
                c == 0x200d || (c >= 0xe0100 && c <= 0xe01ef)) return 0;
            if ((c >= 0x1100 && c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf && c != 0x303f) ||
                (c >= 0xac00 && c <= 0xd7a3) || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xfe30 && c <= 0xfe4f) ||
                (c >= 0xff00 && c <= 0xff60) || (c >= 0xffe0 && c <= 0xffe6) ||
                (c >= 0x1f300 && c <= 0x1f64f) || (c >= 0x1f900 && c <= 0x1f9ff) || (c >= 0x1fa70 && c <= 0x1faff) ||
                (c >= 0x20000 && c <= 0x3fffd)) return 2;
            return 1;
        }
    }
}
