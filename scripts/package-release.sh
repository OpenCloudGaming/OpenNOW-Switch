#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
    printf 'Usage: %s <version> [build-directory]\n' "$0" >&2
    exit 2
fi

version="$1"
build_directory="${2:-switch}"

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+([.-][0-9A-Za-z.-]+)?$ ]]; then
    printf 'Invalid release version: %s\n' "$version" >&2
    exit 2
fi

repo="$(cd "$(dirname "$0")/.." && pwd)"
nro="$repo/build/$build_directory/SwitchNOW.nro"

if [[ ! -f "$nro" ]]; then
    printf 'Missing build artifact: %s. Run the Switch build first.\n' "$nro" >&2
    exit 1
fi

dist="$repo/dist"
package_name="SwitchNOW-$version"
package_root="$dist/$package_name"
switch_directory="$package_root/switch/SwitchNOW"
versioned_nro="$dist/$package_name.nro"
zip_path="$dist/$package_name.zip"

rm -rf "$package_root"
rm -f "$versioned_nro" "$zip_path"
mkdir -p "$switch_directory"

cp "$nro" "$switch_directory/SwitchNOW.nro"
cp "$nro" "$versioned_nro"
cp "$repo/resources/icon/icon.jpg" "$switch_directory/icon.jpg"
cp "$repo/resources/icon/icon.png" "$switch_directory/icon.png"
cp "$repo/README.md" "$package_root/README.md"

cat > "$package_root/INSTALL.txt" <<EOF
SwitchNOW $version

Install:
1. Copy the switch folder from this package to the root of the SD card.
2. Launch Homebrew Menu with title override/application mode.
3. Start SwitchNOW from hbmenu.

Included:
- switch/SwitchNOW/SwitchNOW.nro
- switch/SwitchNOW/icon.jpg
- switch/SwitchNOW/icon.png
- README.md
EOF

(
    cd "$package_root"
    zip -qr "$zip_path" switch README.md INSTALL.txt
)

printf 'Packaged: %s\n' "$zip_path"
printf 'Versioned NRO: %s\n' "$versioned_nro"
