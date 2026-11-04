#!/bin/bash
# Builds sfz123 and installs it to ~/Library/Audio/Plug-Ins/CLAP.
#   scripts/build_and_install.sh            # Apple silicon build
#   scripts/build_and_install.sh universal  # Apple silicon + Intel
set -euo pipefail

cd "$(dirname "$0")/.."

ARCHS="arm64"
if [[ "${1:-}" == "universal" ]]; then
    ARCHS="arm64;x86_64"
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=${ARCHS}"
cmake --build build

DEST="$HOME/Library/Audio/Plug-Ins/CLAP"
mkdir -p "$DEST"
rm -rf "$DEST/sfz123.clap"
cp -R "build/sfz123.clap" "$DEST/"
echo "Installed to $DEST/sfz123.clap"
echo "In REAPER: Options > Preferences > Plug-ins > CLAP, then 'Re-scan' (or restart REAPER)."
