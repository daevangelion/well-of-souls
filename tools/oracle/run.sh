#!/usr/bin/env bash
# tools/oracle/run.sh <script.dsc> <outdir>
#
# Runs the ORIGINAL extracted/Souls.exe under Wine, headless, driven by the .dsc script,
# with the hook DLL injected.  Writes one <label>.txt dump (and a <label>.bmp screenshot)
# per `dump` op into <outdir>.
#
# Determinism: the hook owns the virtual clock, the LCG and the message pump, so two runs
# of the same script produce byte-identical dumps.  Wall-clock time, mouse motion and
# the real keyboard never reach the game.  Any non-determinism left in the dumps is a
# genuine finding, not harness noise, and is reported as a mismatch.
#
# Environment knobs (all optional):
#   WOS_ORACLE_TIMEOUT  seconds to allow the target (default 180)
#   WOS_RANDTRACE       set to any value to log the caller return address of every rand()
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WINE_DIR="${WOS_WINE_DIR:-/mnt/build/wine/wine-11.18-staging-amd64-wow64}"
GAME="${WOS_GAME_DIR:-/mnt/build/wos-oracle}"
BIN=/mnt/build/wos-oracle/bin
PREFIX="${WINEPREFIX:-/mnt/build/wine-prefix}"

[ $# -ge 2 ] || { echo "usage: run.sh <script.dsc> <outdir>" >&2; exit 2; }
DSC="$1"
OUT="$2"
[ -f "$DSC" ] || { echo "run.sh: no such script: $DSC" >&2; exit 2; }
[ -x "$BIN/launcher.exe" ] || "$HERE/build.sh"
[ -x "$WINE_DIR/bin/wine" ] || "$HERE/fetch_wine.sh"

mkdir -p "$OUT"
rm -f "$OUT"/*.txt "$OUT"/*.bmp 2>/dev/null || true

# A clean game directory every run: the original writes save\, temp\ and INI files, and
# a stale soul or a warm scene cache would make run N differ from run 1.
STAGE=/mnt/build/wos-oracle-run
rm -rf "$STAGE"
mkdir -p "$STAGE"
cp -a "$GAME/." "$STAGE/"
cp -f "$BIN/hook.dll" "$STAGE/hook.dll"

export WINEPREFIX="$PREFIX"
export WINEDEBUG="${WINEDEBUG:--all}"
export WINEDLLOVERRIDES="${WINEDLOVERRIDES:-mscoree,mshtml=}"
export TZ=UTC                       # the hook's localtime assumes UTC
export DISPLAY="${DISPLAY:-}"
# The hook is a 32-bit Windows process: a Unix path like /tmp/x.dsc would be resolved
# against whatever drive happens to be current, which is not the Z: drive that maps /.
# winepath is the same conversion the rest of the tooling uses.
winpath() { "$WINE_DIR/bin/winepath" -w "$1" 2>/dev/null; }
export WOS_DSC="$(winpath "$DSC")"
export WOS_OUT="$(winpath "$OUT")"
export WOS_RANDTRACE="${WOS_RANDTRACE:-}"
# Stall reporting is ON by default: it only does anything when the pump counters stop
# moving, and when they do, the counters alone cannot say whether the app is blocked in
# a real GetMessage, inside a long operation, or in a modal box the hook does not own --
# three different fixes.  The report suspends the app's thread briefly, so it is a
# measurement, not part of the run; WOS_STALL=0 turns it off for a timing-sensitive run.
export WOS_STALL="${WOS_STALL:-1}"
# The game calls SetCurrentDirectory(install root) in InitInstance, ~1400 rand()
# calls BEFORE that lands in the launcher's CWD and the rest in the game directory.
# One absolute path keeps the trace in one file.
export WOS_TRACE="${WOS_TRACE:-$(winpath "$PWD/randtrace.txt")}"
# The hook is a 32-bit Windows process: every path it is handed has to be a
# Windows path.  WINE_LOG is the same file as LOG, in Unix form, because `rm`
# is the only thing that has to delete it and it does not speak "Z:\".
LOGPATH="${WOS_LOG:-/tmp/wos-oracle-hook.log}"
export WOS_LOG="$(winpath "$LOGPATH")"

LOG=/tmp/wos-oracle-run.log
: > "$LOG"
rm -f "$LOGPATH"

# Xvfb: Wine needs an X display even with no window manager.  A private one is started per
# run so parallel scripts cannot interfere.
export XDG_RUNTIME_DIR=/tmp/wos-oracle-xdg
mkdir -p "$XDG_RUNTIME_DIR"

# The winepath calls above started a wineserver with no DISPLAY. A target that attaches to
# it gets no graphics driver, every CreateWindow fails and MFC reports "Failed to create
# empty document". Let that server exit so the run starts its own under Xvfb.
"$WINE_DIR/bin/wineserver" -w

xvfb-run -a -s "-screen 0 1024x768x24 -nolisten tcp" \
    "$WINE_DIR/bin/wine" "$BIN/launcher.exe" \
        "$STAGE/Souls.exe" "$STAGE/hook.dll" \
        --timeout "${WOS_ORACLE_TIMEOUT:-180}" \
    >> "$LOG" 2>&1 || true

# The launcher always exits 0 unless it could not start or inject; the dumps are the real
# verdict, so report what happened and let the caller diff.
if [ -s "$LOG" ]; then
    grep -vE 'libEGL|ALSA lib|libpulse|DRI3|GLX|winediag|winemenubuilder|ReadStyleSheet' "$LOG" \
        | grep -E 'err:|launcher:|Fatal|Unhandled|fixme:module' | head -20 >&2 || true
fi

n=$(ls -1 "$OUT"/*.txt 2>/dev/null | wc -l || true)
if [ "$n" -eq 0 ]; then
    echo "run.sh: no dumps were produced; see $LOG" >&2
    if [ -s "$LOGPATH" ]; then
        echo "run.sh: ---- last 30 lines of $LOGPATH ----" >&2
        tail -30 "$LOGPATH" >&2
    else
        echo "run.sh: $LOGPATH is empty: the hook never loaded" >&2
    fi
    exit 1
fi
echo "run.sh: $n dump(s) in $OUT"
