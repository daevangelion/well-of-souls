#!/bin/sh
# Shared version-keyed cache; no root privileges or system package changes.
set -eu
cache="${HOME}/.cache/wos-toolchains"
llvm=llvm-mingw-20250114-msvcrt-ubuntu-20.04-x86_64
sdl=SDL2-2.32.10
mkdir -p "$cache"
# Serialize extraction so another invocation cannot mistake a partial tree for a hit.
exec 9>"$cache/.fetch.lock"
flock 9
stage=
archive=
cleanup() {
    [ -z "$stage" ] || rm -rf "$stage"
    [ -z "$archive" ] || rm -f "$archive"
}
trap cleanup EXIT HUP INT TERM
fetch() {
    name=$1
    url=$2
    marker=$3
    if [ -f "$cache/$name/.wos-complete" ] && [ -f "$cache/$name/$marker" ]; then
        printf 'Using cached %s\n' "$cache/$name"
        return
    fi
    if [ -e "$cache/$name" ]; then
        printf 'Incomplete existing toolchain: %s (move it aside before retrying)\n' "$cache/$name" >&2
        exit 1
    fi
    stage=$(mktemp -d "$cache/.extract.XXXXXX")
    archive="$stage/archive"
    curl --fail --location --retry 3 --output "$archive" "$url"
    tar -xf "$archive" -C "$stage"
    rm -f "$archive"
    archive=
    test -f "$stage/$name/$marker"
    touch "$stage/$name/.wos-complete"
    mv "$stage/$name" "$cache/$name"
    rmdir "$stage"
    stage=
    printf 'Installed %s\n' "$cache/$name"
}
fetch "$llvm" "https://github.com/mstorsjo/llvm-mingw/releases/download/20250114/$llvm.tar.xz" bin/x86_64-w64-mingw32-clang
fetch "$sdl" "https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.tar.gz" x86_64-w64-mingw32/bin/SDL2.dll
