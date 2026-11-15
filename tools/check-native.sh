#!/usr/bin/env bash
# Syntax-checks the native loader with the mingw-w64 cross-compiler (brew install mingw-w64).
# Exits non-zero on any error, so it can gate a push.
set -euo pipefail
cd "$(dirname "$0")/../native"
out="$(x86_64-w64-mingw32-g++ -std=c++17 -fsyntax-only -Wall -Wextra -Wno-cast-function-type -Iinclude \
    -DXBLOADER_EXPORTS -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX \
    -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A000006 src/*.cpp 2>&1 || true)"
if printf '%s' "$out" | grep -qE 'error|warning'; then
    printf '%s\n' "$out" | grep -E 'error|warning'
    exit 1
fi
echo "native code: ok"
