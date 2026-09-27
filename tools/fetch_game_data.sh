#!/bin/sh
# Provision the extracted retail data tree for local runs and CI.
#
# The original "Well of Souls" installer is copyrighted and is deliberately NOT stored
# in this repository. Supply it out-of-band; this script unpacks it into extracted/ via
# the same cicdec flow the project already uses. Nothing here redistributes game content.
#
#   # from a local copy you already have
#   WOS_INSTALLER=/path/to/WellOfSouls.exe tools/fetch_game_data.sh
#
#   # or download a pinned copy (recommended for CI, via a repo secret)
#   WOS_INSTALLER_URL=https://.../WellOfSouls.exe \
#   WOS_INSTALLER_SHA256=<hex> tools/fetch_game_data.sh
#
# Unpacking a Clickteam Install Creator stub needs mono (runs cicdec.exe) and 7z
# (expands the cicdec archive); both are checked below and installed in CI.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
data=${1:-"$root/extracted"}

command -v curl >/dev/null 2>&1 || { echo "fetch_game_data: curl is required" >&2; exit 1; }

installer=${WOS_INSTALLER:-}
if [ -z "$installer" ]; then
    [ -n "${WOS_INSTALLER_URL:-}" ] || {
        echo "fetch_game_data: set WOS_INSTALLER (file) or WOS_INSTALLER_URL (download)" >&2
        exit 2
    }
    installer="$root/work/WellOfSouls.exe"
    mkdir -p "$root/work"
    echo "fetch_game_data: downloading installer"
    curl --fail --location --retry 3 --output "$installer.part" "$WOS_INSTALLER_URL"
    if [ -n "${WOS_INSTALLER_SHA256:-}" ]; then
        printf '%s  %s\n' "$WOS_INSTALLER_SHA256" "$installer.part" | sha256sum --status -c - || {
            rm -f "$installer.part"; echo "fetch_game_data: installer SHA-256 mismatch" >&2; exit 1; }
    fi
    mv "$installer.part" "$installer"
fi
[ -f "$installer" ] || { echo "fetch_game_data: no installer at $installer" >&2; exit 1; }

# The CIC unpacker is a .NET binary; it runs under mono and its archive needs 7z.
command -v mono >/dev/null 2>&1 || {
    echo "fetch_game_data: mono is required to unpack the CIC installer (apt: mono-complete)" >&2; exit 1; }
command -v 7z >/dev/null 2>&1 || {
    echo "fetch_game_data: 7z is required to expand the cicdec archive (apt: p7zip-full)" >&2; exit 1; }

# extract_installer.sh reads assets/WellOfSouls.exe; stage the supplied copy there
# (assets/ is git-ignored, so the copyrighted file never enters the repository).
mkdir -p "$root/assets"
[ "$installer" -ef "$root/assets/WellOfSouls.exe" ] || cp -f "$installer" "$root/assets/WellOfSouls.exe"

"$root/tools/extract_installer.sh" "$data"
test -f "$data/Souls.exe" || { echo "fetch_game_data: extraction did not produce $data/Souls.exe" >&2; exit 1; }
echo "fetch_game_data: data ready at $data"
