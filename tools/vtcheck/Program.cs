using System;
using System.IO;
using System.Text;
using Cexbox;

// Replays a recording (raw terminal output, for example from tools/record_tui.py)
// through VtTerminal in small chunks and prints the final screen.
//
// usage: dotnet run -- <recording> <cols> <rows> [chunk bytes]
if (args.Length < 3)
{
    Console.Error.WriteLine("usage: vtcheck <recording> <cols> <rows> [chunk bytes]");
    return 1;
}

var bytes = File.ReadAllBytes(args[0]);
int cols = int.Parse(args[1]), rows = int.Parse(args[2]);
int chunk = args.Length > 3 ? int.Parse(args[3]) : 7;
var terminal = new VtTerminal(cols, rows);
var replies = new StringBuilder();
terminal.Reply = s => replies.Append(s.Replace("\x1b", "ESC"));

var decoder = new UTF8Encoding(false).GetDecoder();
var chars = new char[bytes.Length * 2];
for (int i = 0; i < bytes.Length; i += chunk)
{
    int n = Math.Min(chunk, bytes.Length - i);
    int c = decoder.GetChars(bytes, i, n, chars, 0, false);
    terminal.Feed(new string(chars, 0, c));
}

Console.WriteLine($"+{new string('-', cols)}+  cursor {terminal.CursorX},{terminal.CursorY}");
for (int y = 0; y < rows; y++)
{
    Console.WriteLine("|" + VtTerminal.RowText(terminal.ViewRow(y, 0)).PadRight(cols) + "|");
}
Console.WriteLine($"+{new string('-', cols)}+");
Console.WriteLine($"scrollback: {terminal.ScrollbackCount}");
Console.WriteLine($"replies: {replies}");
Console.WriteLine($"link: {terminal.LastLink ?? terminal.FindLinkOnScreen() ?? "(none)"}");
return 0;
