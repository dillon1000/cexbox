using System.Collections.Generic;

namespace Cexbox
{
    // The terminal only ever draws characters its bundled font has, so Windows never
    // goes looking for a fallback font. Anything else is swapped for a look-alike.
    static partial class FontCoverage
    {
        const int Unknown = 0x00B7;  // middle dot

        // Characters Claude Code uses that the font lacks, and what to draw instead.
        static readonly Dictionary<int, int> Substitutes = new Dictionary<int, int>
        {
            { 0x2722, '*' }, { 0x2731, '*' }, { 0x2732, '*' }, { 0x2733, '*' },  // spinner stars
            { 0x2736, '*' }, { 0x273B, '*' }, { 0x273D, '*' }, { 0x2734, '*' },
            { 0x2714, 0x2713 }, { 0x2705, 0x2713 }, { 0x2611, 0x2713 },          // check marks
            { 0x2717, 0x00D7 }, { 0x2718, 0x00D7 }, { 0x274C, 0x00D7 },          // crosses
            { 0x2612, 0x00D7 },
            { 0x23F5, 0x25B6 }, { 0x23F4, 0x25C0 },                              // play, reverse
            { 0x23F8, '=' }, { 0x23F9, 0x25A0 },                                 // pause, stop
            { 0x23FA, 0x25CF }, { 0x25EF, 0x25CB },                              // record, large circle
            { 0x23BF, 0x2514 },                                                  // result bracket
            { 0x26A0, '!' }, { 0x26A1, '!' },                                    // warning
            { 0x2610, 0x25A1 }, { 0x29C9, 0x25A1 },                              // empty box, joined squares
            { 0x2139, 'i' },                                                     // information
        };

        public static bool Has(int codePoint)
        {
            int lo = 0, hi = Ranges.Length / 2 - 1;
            while (lo <= hi)
            {
                int mid = (lo + hi) / 2;
                if (codePoint < Ranges[mid * 2]) hi = mid - 1;
                else if (codePoint > Ranges[mid * 2 + 1]) lo = mid + 1;
                else return true;
            }
            return false;
        }

        // The character to draw for codePoint: itself if the font has it.
        public static int Resolve(int codePoint)
        {
            if (Has(codePoint)) return codePoint;
            if (Substitutes.TryGetValue(codePoint, out int other) && Has(other)) return other;
            return Unknown;
        }
    }
}
