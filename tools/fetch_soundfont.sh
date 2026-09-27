#!/bin/sh
# TimGM6mb GPL-2 soundfont data; retain its upstream copyright/license alongside it.
set -eu
cache="$HOME/.cache/wos-soundfont"
base=https://raw.githubusercontent.com/arbruijn/TimGM6mb/d6ad4ed72dce1fd3d67f17b74e08cd7ae7941a96
mkdir -p "$cache"
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
