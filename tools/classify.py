"""Split a program's imports into allowed and not allowed for Xbox (UWP) apps.

The allow-list comes from mingw-w64's WindowsApp definition files
(mingw-w64-crt/lib-common, every library listed in windowsapp.mri).

usage: python3 classify.py <defs dir> imports.tsv [delay.tsv ...]
Each .tsv line is DLL<TAB>function.
"""
import os
import re
import sys
from collections import defaultdict

defs_dir, *import_files = sys.argv[1:]

allowed = set()
for name in os.listdir(defs_dir):
    in_exports = False
    for line in open(os.path.join(defs_dir, name), errors="ignore"):
        line = line.split(";")[0].strip()
        if not line:
            continue
        if line.upper().startswith("EXPORTS"):
            in_exports = True
            continue
        if line.upper().startswith(("LIBRARY", "#")):
            continue
        if in_exports:
            sym = re.split(r"[\s=@]", line)[0]
            if sym:
                allowed.add(sym)

blocked = defaultdict(list)
ok = 0
total = 0
for path in import_files:
    for line in open(path):
        dll, fn = line.rstrip("\n").split("\t")
        total += 1
        if fn in allowed:
            ok += 1
        else:
            blocked[dll.lower()].append(fn)

print(f"allowed names known: {len(allowed)}")
print(f"imports: {total}  allowed: {ok}  not allowed: {total - ok}")
for dll in sorted(blocked, key=lambda d: -len(blocked[d])):
    fns = sorted(blocked[dll])
    print(f"\n{dll} ({len(fns)}): " + " ".join(fns))
