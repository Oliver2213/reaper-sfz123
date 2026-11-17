#!/bin/bash
# Builds a universal sfz123.clap, signs it with a Developer ID, notarizes and staples it,
# and zips it with the license, third-party notices and readme into dist/sfz123-<version>-mac.zip.
#   scripts/release.sh
#   SFZ123_SKIP_NOTARIZE=1 scripts/release.sh   # sign only, for a local check
#
# Environment:
#   SFZ123_SIGN_ID          signing identity (default: the first "Developer ID Application" in the keychain)
#   SFZ123_NOTARY_PROFILE   notarytool keychain profile (default: sfz123). Create it once with
#                           xcrun notarytool store-credentials sfz123 --apple-id <email> --team-id <team>
set -euo pipefail

cd "$(dirname "$0")/.."

VERSION=$(sed -n 's/^project(sfz123 VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
SIGN_ID="${SFZ123_SIGN_ID:-$(security find-identity -v -p codesigning \
    | sed -n 's/.*"\(Developer ID Application: .*\)"/\1/p' | head -n 1)}"
PROFILE="${SFZ123_NOTARY_PROFILE:-sfz123}"
if [[ -z "$SIGN_ID" ]]; then
    echo "No Developer ID Application identity found; set SFZ123_SIGN_ID" >&2
    exit 1
fi
if [[ -n "$(git status --porcelain)" ]]; then
    echo "Warning: the working tree has uncommitted changes" >&2
fi

rm -rf build-release
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
cmake --build build-release --target sfz123

BUNDLE=build-release/sfz123.clap
codesign --force --options runtime --timestamp --sign "$SIGN_ID" "$BUNDLE"
codesign --verify --strict --verbose=2 "$BUNDLE"

if [[ -z "${SFZ123_SKIP_NOTARIZE:-}" ]]; then
    ditto -c -k --keepParent "$BUNDLE" build-release/notarize.zip
    xcrun notarytool submit build-release/notarize.zip --keychain-profile "$PROFILE" --wait
    xcrun stapler staple "$BUNDLE"
    spctl --assess --type install --verbose "$BUNDLE" || true
fi

STAGE="build-release/sfz123-$VERSION"
mkdir -p "$STAGE" dist
cp -R "$BUNDLE" "$STAGE/"
cp LICENSE "$STAGE/LICENSE.txt"
cp README.md "$STAGE/README.md"
scripts/third_party_notices.sh "$STAGE/THIRD_PARTY_NOTICES.txt"
ZIP="dist/sfz123-$VERSION-mac.zip"
rm -f "$ZIP"
ditto -c -k --keepParent "$STAGE" "$ZIP"
echo "Built $ZIP ($(lipo -archs "$BUNDLE/Contents/MacOS/sfz123"))"
