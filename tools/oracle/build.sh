#!/usr/bin/env bash
# tools/oracle/build.sh -- build the i686 hook DLL and the injector launcher.
#
# Uses the cached llvm-mingw already in the tree toolchain cache (msvcrt flavour, so the
# hook needs no VC runtime of its own and stays self-contained).  Output goes to
# /mnt/build/wos-oracle/bin so nothing lands in the repo tree.
set -euo pipefail

TC="${WOS_MINGW:-$HOME/.cache/wos-toolchains/llvm-mingw-20250114-msvcrt-ubuntu-20.04-x86_64}"
BIN=/mnt/build/wos-oracle/bin
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/hook"
CC="$TC/bin/i686-w64-mingw32-clang"

TARGET="-target i686-w64-windows-gnu"
if [ ! -x "$CC" ] && command -v i686-w64-mingw32-gcc >/dev/null 2>&1; then
    # apt's gcc-mingw-w64-i686 (what tools/bootstrap_cloud.sh installs). gcc has no
    # -target, and libgcc must be static or hook.dll imports libgcc_s_dw2-1.dll, which
    # the target process cannot find (LoadLibrary error 126 at injection).
    CC=i686-w64-mingw32-gcc
    TARGET="-static-libgcc"
fi
[ -x "$CC" ] || command -v "$CC" >/dev/null 2>&1 || { echo "build.sh: no i686 compiler (llvm-mingw clang at $TC or i686-w64-mingw32-gcc)" >&2; exit 1; }
mkdir -p "$BIN"

COMMON="$TARGET -O2 -fno-exceptions -fno-stack-protector
         -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type
         -DPSAPI_VERSION=1"

# The hook must be a plain Win32 DLL with no CRT startup surprises.
"$CC" $COMMON -shared -o "$BIN/hook.dll" "$SRC/hook.c" \
    -Wl,--out-implib,"$BIN/hook.lib" -lkernel32 -luser32 -lmsvcrt -lgdi32
"$CC" $COMMON -o "$BIN/launcher.exe" "$SRC/launcher.c" -lkernel32 -luser32

echo "built:"
file "$BIN/hook.dll" "$BIN/launcher.exe" | sed 's/^/  /'
