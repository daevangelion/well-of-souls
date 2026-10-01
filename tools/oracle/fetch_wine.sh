#!/usr/bin/env bash
# tools/oracle/fetch_wine.sh -- provision a portable Wine that runs Souls.exe (32-bit PE).
#
# Idempotent and cached: every step checks for its output first, so re-running is free.
# Nothing is installed system-wide; Wine, its prefix and the game copy all live under
# /mnt/build (the repo disk has only a few GB free).
#
# Layout produced:
#   /mnt/build/wine/wine-<ver>-staging-amd64-wow64/   the Wine build itself
#   /mnt/build/wine-prefix/                          win64 prefix (new WoW64)
#   /mnt/build/wos-oracle/                           a COPY of extracted/, the game dir
#   /mnt/build/wine-dl/                              download cache
set -euo pipefail

WINE_VER="${WINE_VER:-11.18}"
DL=/mnt/build/wine-dl
WINE_ROOT=/mnt/build/wine
WINE_DIR="$WINE_ROOT/wine-${WINE_VER}-staging-amd64-wow64"
PREFIX=/mnt/build/wine-prefix
GAME=/mnt/build/wos-oracle
REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

say() { printf '[fetch_wine] %s\n' "$*" >&2; }
die() { printf '[fetch_wine] FATAL: %s\n' "$*" >&2; exit 1; }

fetch() { # fetch <url> <file> <sha256>
    local url="$1" out="$2" want="$3" got
    if [ -f "$out" ]; then
        got=$(sha256sum "$out" | cut -d' ' -f1)
        if [ "$got" = "$want" ]; then say "cached $(basename "$out")"; return 0; fi
        say "checksum mismatch on $(basename "$out"), refetching"; rm -f "$out"
    fi
    say "fetching $(basename "$out")"
    curl -fsSL --retry 3 -o "$out" "$url" || die "download failed: $url"
    got=$(sha256sum "$out" | cut -d' ' -f1)
    [ "$got" = "$want" ] || die "sha256 mismatch for $out: got $got want $want"
}

# ---------------------------------------------------------------- 1. the Wine build
mkdir -p "$DL" "$WINE_ROOT"
if [ ! -x "$WINE_DIR/bin/wine" ]; then
    fetch "https://github.com/Kron4ek/Wine-Builds/releases/download/${WINE_VER}/sha256sums.txt" \
          "$DL/sha256sums.txt" \
          "$(curl -fsSL "https://github.com/Kron4ek/Wine-Builds/releases/download/${WINE_VER}/sha256sums.txt" | grep "wine-${WINE_VER}-staging-amd64-wow64.tar.xz " | cut -d' ' -f1)"
    fetch "https://github.com/Kron4ek/Wine-Builds/releases/download/${WINE_VER}/wine-${WINE_VER}-staging-amd64-wow64.tar.xz" \
          "$DL/wine-${WINE_VER}-staging-amd64-wow64.tar.xz" \
          "$(grep "wine-${WINE_VER}-staging-amd64-wow64.tar.xz " "$DL/sha256sums.txt" | cut -d' ' -f1)"
    say "unpacking Wine $WINE_VER (staging, wow64)"
    tar -xJf "$DL/wine-${WINE_VER}-staging-amd64-wow64.tar.xz" -C "$WINE_ROOT"
else
    say "cached Wine $WINE_DIR"
fi
[ -x "$WINE_DIR/bin/wine" ] || die "wine missing at $WINE_DIR/bin/wine"

export WINEPREFIX="$PREFIX"
if [ ! -f "$PREFIX/system.reg" ]; then
    say "creating the win64 prefix at $PREFIX (new WoW64: 32-bit apps get drive_c/windows/syswow64)"
    WINEARCH=win64 WINEDEBUG=-all "$WINE_DIR/bin/wineboot" -u >/dev/null 2>&1 || die "wineboot failed"
fi

# ---------------------------------------------------------------- 2. MFC42
# Souls.exe imports MFC42.DLL by ordinal and it is not shipped in extracted/.  The
# redistributable is the same one winetricks' vcrun6 verb uses; the outer exe is a
# self-extracting archive and the inner vcredist.exe holds a CAB, which 7z reads.
MFC_URL="https://download.microsoft.com/download/vc60pro/Update/2/W9XNT4/EN-US/VC6RedistSetup_deu.exe"
MFC_SHA="c2eb91d9c4448d50e46a32fecbcc3b418706d002beab9b5f4981de552098cee7"
CABDIR="$DL/vcrun6/cab"
if [ -f "$PREFIX/drive_c/windows/syswow64/mfc42.dll" ] && [ -f "$PREFIX/drive_c/windows/system32/mfc42.dll" ]; then
    say "cached mfc42.dll in the prefix"
else
    mkdir -p "$DL/vcrun6"
    fetch "$MFC_URL" "$DL/vcrun6/VC6RedistSetup_deu.exe" "$MFC_SHA"
    if [ ! -f "$DL/vcrun6/ex/vcredist.exe" ]; then
        mkdir -p "$DL/vcrun6/ex"
        7z x -o"$DL/vcrun6/ex" "$DL/vcrun6/VC6RedistSetup_deu.exe" >/dev/null || die "cannot unpack VC6RedistSetup_deu.exe"
    fi
    # vcredist.exe is a PE carrying an embedded CAB under RCDATA; 7z reads it directly,
    # so no wine, no cabextract.
    mkdir -p "$CABDIR"
    7z x -o"$CABDIR" "$DL/vcrun6/ex/vcredist.exe" >/dev/null || die "cannot unpack vcredist.exe"
    for d in "$PREFIX/drive_c/windows/system32" "$PREFIX/drive_c/windows/syswow64"; do
        mkdir -p "$d"
        # msvcrt/oleaut32 in the same CAB are deliberately NOT installed: Wine's own
        # builds are the ones the rest of the prefix expects.
        cp -f "$CABDIR/mfc42.dll" "$CABDIR/mfc42u.dll" "$CABDIR/msvcp60.dll" "$d/"
    done
    say "installed mfc42.dll, mfc42u.dll, msvcp60.dll into system32 and syswow64"
fi

# ---------------------------------------------------------------- 3. the game copy
if [ ! -f "$GAME/Souls.exe" ]; then
    say "copying extracted/ to $GAME (the original never runs from the repo tree)"
    mkdir -p "$GAME"
    cp -a "$REPO/extracted/." "$GAME/"
fi

# ---------------------------------------------------------------- 4. registry
# A stable prefix per run: Disable registry writes would throw away the profile the
# game reads at boot (Preferences, Debug, currentThemeName), and the game writes INI
# files under the game dir anyway.
REG=/tmp/wos-oracle-reg.reg
cat > "$REG" <<'EOF'
REGEDIT4

[HKEY_CURRENT_USER\Software\Wine\Direct3D]
"csmt"=dword:00000000
"VideoMemorySize"=dword:00000100

[HKEY_CURRENT_USER\Software\Wine\WineDbg]
"ShowCrashDialog"=dword:00000000

[HKEY_CURRENT_USER\Software\Wine\X11 Driver]
"Decorated"="N"
"Managed"="N"
EOF
if ! grep -q 'WOS-ORACLE-REGISTRY' "$PREFIX/user.reg" 2>/dev/null; then
    say "applying registry tweaks"
    WINEDEBUG=-all "$WINE_DIR/bin/wine" regedit "$REG" >/dev/null 2>&1 || true
    printf '\n; WOS-ORACLE-REGISTRY\n' >> "$PREFIX/user.reg"
fi

say "ready: $WINE_DIR/bin/wine  prefix=$PREFIX  game=$GAME"
