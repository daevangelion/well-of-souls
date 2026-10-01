#!/bin/sh
# tools/bootstrap_cloud.sh -- prepare a fresh Claude Code cloud container (Ubuntu 24.04,
# root, egress through the agent proxy) for work on this repo. Idempotent: every step
# checks for its result first, so re-running is cheap.
#
#   tools/bootstrap_cloud.sh               # everything: deps, game data, build, decomp, oracle
#   tools/bootstrap_cloud.sh --no-oracle   # skip Wine + the oracle hook (saves ~1 GB, ~3 min)
#   tools/bootstrap_cloud.sh --no-decomp   # skip radare2/r2ghidra and work/decomp/all.c (~10 min)
#
# What it sets up, and why each piece comes from where it does:
#   1. apt: build deps (SDL2), and for the oracle i386 Wine 9.0, mingw i686 gcc, Xvfb, 7z.
#      GitHub release downloads are blocked by the proxy, so the pinned Kron4ek Wine and
#      llvm-mingw that tools/oracle/* name by default are not reachable; apt replaces both.
#   2. assets/WellOfSouls.exe from the developer's site, unpacked into extracted/ by
#      tools/cic_reference.py (pure Python; cicdec needs mono, which is not installed).
#   3. build/wos and the self-tests.
#   4. radare2 + r2ghidra built from git (anonymous git reads of public repos DO work),
#      then work/decomp/all.c via tools/r2/decomp.py. Ghidra itself is not reachable.
#   5. The oracle: system Wine linked where tools/oracle/run.sh looks for it, the prefix,
#      MFC42 and the game copy (tools/oracle/fetch_wine.sh), and the hook (build.sh).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
oracle=1 decomp=1
for a in "$@"; do
    case "$a" in
    --no-oracle) oracle=0 ;;
    --no-decomp) decomp=0 ;;
    -h|--help) sed -n '2,25p' "$0"; exit 0 ;;
    *) echo "bootstrap: unknown option $a" >&2; exit 2 ;;
    esac
done
say() { printf '[bootstrap] %s\n' "$*" >&2; }
need_root() { [ "$(id -u)" = 0 ] || { echo "bootstrap: run as root (apt and /usr/local installs)" >&2; exit 1; }; }
export DEBIAN_FRONTEND=noninteractive

# ------------------------------------------------------------------ 1. packages
need_root
pkgs="build-essential cmake pkg-config git curl python3 libsdl2-dev"
[ $oracle = 1 ] && pkgs="$pkgs wine wine64 wine32:i386 gcc-mingw-w64-i686 xvfb xauth p7zip-full"
[ $decomp = 1 ] && pkgs="$pkgs meson ninja-build"
missing=
for p in $pkgs; do dpkg -s "$p" >/dev/null 2>&1 || missing="$missing $p"; done
if [ -n "$missing" ]; then
    if [ $oracle = 1 ] && ! dpkg --print-foreign-architectures | grep -qx i386; then
        say "enabling the i386 architecture (wine32)"
        dpkg --add-architecture i386
    fi
    say "apt:$missing"
    apt-get update -q >/dev/null
    apt-get install -y -q $missing >/dev/null
else
    say "packages present"
fi

# ------------------------------------------------------------------ 2. game data
if [ ! -f "$root/extracted/Souls.exe" ]; then
    if [ ! -f "$root/assets/WellOfSouls.exe" ]; then
        say "downloading the installer (assets/ is gitignored, never commit it)"
        mkdir -p "$root/assets"
        curl -fsSL --retry 3 -o "$root/assets/WellOfSouls.exe.part" http://www.synthetic-reality.us/WellOfSouls.exe
        mv "$root/assets/WellOfSouls.exe.part" "$root/assets/WellOfSouls.exe"
    fi
    say "unpacking into extracted/"
    python3 "$root/tools/cic_reference.py" "$root/assets/WellOfSouls.exe" "$root/extracted" >/dev/null
fi
[ -f "$root/extracted/Souls.exe" ] || { echo "bootstrap: extraction failed" >&2; exit 1; }
say "game data ready"

# ------------------------------------------------------------------ 3. build
cmake -S "$root" -B "$root/build" >/dev/null
cmake --build "$root/build" -j"$(nproc)" >/dev/null
say "built $root/build/wos"

# ------------------------------------------------------------------ 4. decompiler
if [ $decomp = 1 ]; then
    src=/mnt/build/src
    mkdir -p "$src"
    if ! command -v r2 >/dev/null 2>&1; then
        say "building radare2 from git (~3 min)"
        [ -d "$src/radare2" ] || git clone -q --depth 1 https://github.com/radareorg/radare2 "$src/radare2"
        (cd "$src/radare2" && ./configure --prefix=/usr/local >/dev/null && make -j"$(nproc)" >/dev/null && make install >/dev/null)
        ldconfig
    fi
    if ! r2 -qc Lc -- 2>/dev/null | grep -q r2ghidra; then
        say "building r2ghidra from git (~5 min)"
        [ -d "$src/r2ghidra" ] || git clone -q https://github.com/radareorg/r2ghidra "$src/r2ghidra"
        (cd "$src/r2ghidra" && ./preconfigure >/dev/null 2>&1; ./configure --prefix=/usr/local >/dev/null && make -j"$(nproc)" >/dev/null && make install >/dev/null)
    fi
    r2 -qc Lc -- 2>/dev/null | grep -q r2ghidra || { echo "bootstrap: r2ghidra did not install" >&2; exit 1; }
    if [ ! -s "$root/work/decomp/all.c" ]; then
        say "decompiling all labels.csv functions into work/decomp/all.c (~5 min)"
        python3 "$root/tools/r2/decomp.py" >/dev/null
    fi
    say "decompiler ready: tools/r2/decomp.py <addr|name> ...; work/decomp/all.c"
fi

# ------------------------------------------------------------------ 5. oracle
if [ $oracle = 1 ]; then
    # tools/oracle/run.sh and fetch_wine.sh look for a Wine tree at this path; point it at
    # the system Wine (its bin/ needs wine, wineboot, winepath, wineserver, regedit).
    wdir=/mnt/build/wine/wine-11.18-staging-amd64-wow64
    if [ ! -x "$wdir/bin/wine" ]; then
        mkdir -p /mnt/build/wine-sys/bin /mnt/build/wine
        for t in wine wineboot winepath wineserver regedit; do
            ln -sf "$(command -v $t)" /mnt/build/wine-sys/bin/$t
        done
        ln -sfn /mnt/build/wine-sys "$wdir"
    fi
    bash "$root/tools/oracle/fetch_wine.sh" 2>&1 | sed 's/^/  /' >&2
    # fetch_wine.sh's wineboot leaves a wineserver with no DISPLAY behind; a run that
    # attached to it would get no graphics driver.
    WINEPREFIX=/mnt/build/wine-prefix wineserver -k 2>/dev/null || true
    bash "$root/tools/oracle/build.sh" >/dev/null
    say "oracle ready: tools/oracle/run.sh <dsc> <outdir>; tests/diff_oracle.sh (WOS_BUILD=$root/build)"
fi

say "done. Acceptance: SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy tests/replay_offline.sh"
