#!/bin/sh
# Soundfont provisioning for the desktop and Android builds.
#
# Default: fetch the pinned TimGM6mb bank (GPL-2) into ~/.cache/wos-soundfont.
#
# Override to use your own bank by supplying a file or a URL. It is staged under the
# same "TimGM6mb.sf2" filename the engine loads at runtime (a fixed slot), so a custom
# bank works without touching the engine:
#   WOS_SOUNDFONT=/path/to/font.sf2 tools/fetch_soundfont.sh
#   WOS_SOUNDFONT_URL=https://... WOS_SOUNDFONT_SHA256=<hex> tools/fetch_soundfont.sh
#   WOS_SOUNDFONT_LICENSE=/path/to/LICENSE   # optional license text for a custom bank
set -eu
cache="$HOME/.cache/wos-soundfont"
base=https://raw.githubusercontent.com/arbruijn/TimGM6mb/d6ad4ed72dce1fd3d67f17b74e08cd7ae7941a96
mkdir -p "$cache"

# ---- custom bank (file or URL) overrides the pinned default --------------------
if [ -n "${WOS_SOUNDFONT:-}" ] || [ -n "${WOS_SOUNDFONT_URL:-}" ]; then
    src=${WOS_SOUNDFONT:-}
    if [ -n "$src" ]; then
        [ -f "$src" ] || { echo "fetch_soundfont: no soundfont at $src" >&2; exit 1; }
    else
        src="$cache/.custom.sf2"
        curl --fail --location --retry 3 --output "$src.part" "$WOS_SOUNDFONT_URL"
        if [ -n "${WOS_SOUNDFONT_SHA256:-}" ]; then
            printf '%s  %s\n' "$WOS_SOUNDFONT_SHA256" "$src.part" | sha256sum --status -c - \
                || { rm -f "$src.part"; echo "fetch_soundfont: soundfont SHA-256 mismatch" >&2; exit 1; }
        fi
        mv "$src.part" "$src"
    fi
    cp -f "$src" "$cache/TimGM6mb.sf2"
    # Never ship TimGM6mb's GPL-2 COPYING for a custom bank: use the user's license if
    # given, else a short notice that the bank is user-supplied and under its own terms.
    if [ -n "${WOS_SOUNDFONT_LICENSE:-}" ] && [ -f "$WOS_SOUNDFONT_LICENSE" ]; then
        cp -f "$WOS_SOUNDFONT_LICENSE" "$cache/TimGM6mb.LICENSE.txt"
    else
        printf 'User-supplied soundfont: %s\nDistributed under its own terms; see the soundfont provider.\n' \
            "${WOS_SOUNDFONT_URL:-$src}" > "$cache/TimGM6mb.LICENSE.txt"
    fi
    printf 'Soundfont: %s/TimGM6mb.sf2 (custom)\n' "$cache"
    exit 0
fi

# ---- default: pinned TimGM6mb -------------------------------------------------
fetch() {
    source=$1 dest=$2 sha=$3
    if [ -f "$dest" ] && printf '%s  %s\n' "$sha" "$dest" | sha256sum --status -c -; then
        return
    fi
    curl --fail --location --retry 3 --output "$dest.part" "$base/$source"
    printf '%s  %s\n' "$sha" "$dest.part" | sha256sum -c -
    mv "$dest.part" "$dest"
}
fetch TimGM6mb.sf2 "$cache/TimGM6mb.sf2" c5378b62028c920cb11e4803327983fee2f2cdff5dc89c708e39da417e51c854
fetch COPYING.txt "$cache/TimGM6mb.LICENSE.txt" caf4761ca5e96fd034502a4dfac59f65fce61f3e5b4801b0b149c900fee29f06
printf 'Soundfont: %s/TimGM6mb.sf2\n' "$cache"
