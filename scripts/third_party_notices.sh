#!/bin/bash
# Writes the licenses of the third-party code compiled into sfz123 to one file.
#   scripts/third_party_notices.sh THIRD_PARTY_NOTICES.txt
set -euo pipefail

cd "$(dirname "$0")/.."
OUT="$1"

S=third_party/sfizz
# name|license file, for everything sfizz builds into the plug-in, and the CLAP headers
COMPONENTS=(
    "sfizz|$S/LICENSE"
    "abseil-cpp|$S/external/abseil-cpp/LICENSE"
    "atomic_queue|$S/external/atomic_queue/LICENSE"
    "cephes|$S/external/cephes/LICENSE.txt"
    "ghc::filesystem|$S/external/filesystem/LICENSE"
    "fmidi|$S/external/fmidi/LICENSE.md"
    "invoke.hpp|$S/external/invoke.hpp/LICENSE.md"
    "jsl|$S/external/jsl/LICENSE.md"
    "SIMDe|$S/external/simde/COPYING"
    "st_audiofile|$S/external/st_audiofile/LICENSE.md"
    "dr_libs|$S/external/st_audiofile/thirdparty/dr_libs/LICENSE"
    "libaiff|$S/external/st_audiofile/thirdparty/libaiff/LICENSE"
    "stb_vorbis|$S/external/st_audiofile/thirdparty/stb_vorbis/LICENSE"
    "WavPack|$S/external/st_audiofile/thirdparty/wavpack/COPYING"
    "cpuid|$S/src/external/cpuid/LICENSE.rst"
    "hiir|$S/src/external/hiir/license.txt"
    "kiss_fft|$S/src/external/kiss_fft/COPYING"
    "pugixml|$S/src/external/pugixml/LICENSE.md"
    "spline|$S/src/external/spline/LICENSE"
    "Surge Synth Team Tunings|$S/src/external/tunings/LICENSE.md"
    "CLAP|third_party/clap/LICENSE"
)

section() {
    printf '\n\n================================================================================\n%s\n================================================================================\n\n' "$1"
}

{
    echo "sfz123 is GPL-3.0-or-later (LICENSE.txt). Its SoundFont engine, sfl2, is from SFLT by"
    echo "ash taylor, also GPL-3.0. Its reverb follows Freeverb by Jezar at Dreampoint (public domain)."
    echo "sfz123 also contains the following software, under these licenses."

    for c in "${COMPONENTS[@]}"; do
        section "${c%%|*}"
        cat "${c#*|}"
    done

    # ThreadPool has only an SPDX tag; this is the zlib license text it refers to
    section "ThreadPool"
    cat <<'EOF'
Copyright (c) 2012 Jakob Progsch, Václav Zeman

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
EOF

    # Rust crates, from the cargo registry; MIT is chosen where a crate offers it
    cargo metadata --format-version 1 --manifest-path engine-sf2/Cargo.toml 2>/dev/null | python3 -c '
import json, sys, os
for p in json.load(sys.stdin)["packages"]:
    if p["source"]:
        print(p["name"], p["version"], p["license"], os.path.dirname(p["manifest_path"]), sep="|")
' | while IFS='|' read -r name version license dir; do
        file=$(ls "$dir" | grep -iE '^licen[cs]e-mit' | head -n 1 || true)
        [[ -n "$file" ]] || file=$(ls "$dir" | grep -iE '^(licen[cs]e|copying)' | head -n 1 || true)
        [[ -n "$file" ]] || { echo "No license file for crate $name" >&2; exit 1; }
        section "$name $version (Rust crate, $license)"
        cat "$dir/$file"
    done
} > "$OUT"
