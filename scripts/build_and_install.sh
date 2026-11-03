#!/bin/bash
# Builds SF Player and installs it to ~/Library/Audio/Plug-Ins/CLAP.
#   scripts/build_and_install.sh            # Apple silicon build
#   scripts/build_and_install.sh universal  # Apple silicon + Intel
set -euo pipefail

cd "$(dirname "$0")/.."

ARCHS="arm64"
if [[ "${1:-}" == "universal" ]]; then
    ARCHS="arm64;x86_64"
    rustup target add x86_64-apple-darwin aarch64-apple-darwin >/dev/null 2>&1 || true
fi

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=${ARCHS}"
cmake --build build

DEST="$HOME/Library/Audio/Plug-Ins/CLAP"
mkdir -p "$DEST"
rm -rf "$DEST/SF Player.clap"
cp -R "build/SF Player.clap" "$DEST/"
echo "Installed to $DEST/SF Player.clap"
echo "In REAPER: Options > Preferences > Plug-ins > CLAP, then 'Re-scan' (or restart REAPER)."
