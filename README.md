# sfz123

An accessible SF2, SF3 and SFZ instrument player for REAPER on macOS, as a CLAP plugin.

By Blake Oliver. Source: <https://github.com/Oliver2213/reaper-sfz123>

- **SF2 / SF3** SoundFonts play through `sfl2`, the synthesis core from SFLT (vendored in `vendor/sfl2`).
- **SFZ** instruments play through [sfizz](https://sfz.tools/sfizz/), which supports SFZ v2 and the ARIA extensions.
- The editor uses native AppKit controls, so VoiceOver, Full Keyboard Access and system text sizes work. Every setting is also a normal CLAP parameter, so it appears in REAPER's generic parameter list, in automation envelopes, and in OSARA's parameter dialog.

## Install

```sh
scripts/build_and_install.sh            # Apple silicon
scripts/build_and_install.sh universal  # Apple silicon and Intel
```

Clone with submodules (`git clone --recursive`, or `git submodule update --init --recursive` afterwards). You need Xcode command line tools, CMake, Ninja and Rust. The script installs `sfz123.clap` into `~/Library/Audio/Plug-Ins/CLAP`. In REAPER, re-scan CLAP plug-ins (Preferences > Plug-ins > CLAP) or restart REAPER, then add **sfz123** to a track. The plug-in used to be called SF Player; projects that use it load unchanged.

## Using it

Open the plug-in window and choose **Open…**. You can also drop a file on the window, or pick one from the **Recent** menu. **Previous File** and **Next File** step through the other instruments in the same folder, which is a quick way to audition a folder of SoundFonts. VoiceOver announces when a file finishes loading, including the instrument's name and size, and announces errors too. If a load fails, the previous instrument keeps playing.

The window has these parts, from top to bottom:

| Part | What it does |
| --- | --- |
| File buttons | Open, Recent, Previous File, Next File, Reload (re-reads an edited SFZ), Show in Finder, Unload |
| Instrument, path and status | Name and format, the file path, and the load status. Errors and unsupported SFZ opcodes are reported here. |
| Preset (SF2/SF3 only) | Every preset as `bank:program name (keys)`, for example `000:047  Timpani  (A1 to C4)`, with Previous Preset and Next Preset buttons. Presets that can't make any sound (empty, or only missing, ROM or silent samples) are hidden, and the status line says how many. If no preset passes that check, all of them are listed instead. |
| Instrument controls tab | The controls an SFZ defines, such as Splendid Grand's Resonance, Release and Veltrack. For SoundFonts: modulation, expression, sustain, reverb send and chorus send. |
| Sound tab | Volume, transpose, fine tune, velocity curve, polyphony, interpolation quality |
| SoundFont tabs | MIDI channel mode, program-change following, bend range, mono legato and glide, envelope, filter and vibrato overrides, reverb and chorus. These are dimmed when an SFZ is loaded. |

Each slider has a text field next to it. Type a value such as `-6`, `250 ms`, `1.5 s` or `default`, then press Return. On a focused slider, the arrow keys move one step, Shift-arrow or Page Up/Down moves ten steps, and Home/End jump to the ends. VoiceOver increment and decrement use the same step sizes, and values are read with units, for example "−6.0 dB" or "Instrument default".

### REAPER specifics

- **Project recall**: the instrument and every setting are saved in the project. The instrument path is stored relative to the project folder, so a project folder can be moved or shared. If the file isn't at that relative path, REAPER's own media search (`resolve_fn2`) looks for it, the same way REAPER finds moved audio. The preset is saved by bank and program and instrument controls by CC number, so neither drifts if an instrument is edited. A file that can't be found is reported in the status line, and the saved path is kept.
- **Project media**: sfz123 tells REAPER which files the loaded instrument uses: the SoundFont, or the `.sfz` plus every sample and `#include`. *Save project as…* with *Copy all media into project directory* (or *Move*) then copies the instrument into the project, applying your REAPER media settings and exclusions. SoundFonts go in `SoundFonts/`, and SFZ instruments go in `SFZ/<instrument folder>/` with their folder layout intact, so they keep working. sfz123 switches to the copy right away and the project stores the new relative path. These files also appear in the Project Bay, and *Clean current project directory* won't delete them. Only the instrument currently loaded counts, so files you previewed with Next File and moved past are not copied.
- **Where is my instrument?** Under the file path, the plug-in window says whether the instrument is stored in the project folder or outside it, and how to copy it in.
- **REAPER presets, FX chains and track templates**: REAPER doesn't tell plug-ins when it is saving a preset rather than a project, so these store the instrument path relative to the project that was open when you saved them. Used in a project in another folder, that path usually points to the wrong place, and sfz123 falls back to REAPER's media search, which only finds the file if it is in the project or one of REAPER's media folders. If the instrument isn't found, the status line says so; use Open or Recent to load it again. A project that has never been saved has no folder to be relative to, so anything saved from it stores an absolute path. Hosts that do report preset saves (CLAP's state-context extension) always get an absolute path.
- **FX and track named after the instrument**: the FX takes the instrument's name, plus the preset for SoundFonts, for example "MuseScore General: Violin (sfz123)", so the FX chain, mixer and OSARA say what's loaded, and it follows preset changes. A track that still has REAPER's automatic name (or none) gets the same name, which is what you hear when arrowing through tracks; with several sfz123 on one track, the first one names it. Names you type yourself are never replaced. Turn this off with *Name the FX and track after the instrument* in the plug-in window; the setting applies to every sfz123.
- **Automation**: every parameter can be automated. Instrument controls are renamed after the loaded instrument (for example "Resonance (CC 70)"), and unused slots are hidden.
- **MIDI controllers**: incoming CCs move the matching control, so the window and REAPER's parameter list stay in sync.
- **Playing MIDI files (General MIDI)**: set *MIDI channel mode* to *General MIDI multitimbral*. Each channel then follows its own bank select and program changes, and channel 10 plays drums.
- **Program changes**: in single-preset mode, MIDI program changes and bank selects pick the preset when *Follow program changes* is on. The Preset parameter follows along.
- **Rendering**: during offline renders, SFZ instruments wait for samples to stream in (so there are no dropouts) and use sfizz's best interpolation.
- **Note names**: SFZ key labels and keyswitches are published through CLAP note names.
- Pitch bend, channel and polyphonic aftertouch, the sustain and sostenuto pedals, RPN pitch bend range and tuning, and GM/GS reset SysEx are all handled.

## How it is built

```
src/plugin.cpp      CLAP plugin: parameters, state, events, engine hand-off between threads
src/engine_sf2.cpp  wrapper over the Rust SF2/SF3 engine (C ABI: include/sf2engine.h)
src/engine_sfz.cpp  wrapper over sfizz
src/gui_mac.mm      AppKit editor
src/params.cpp      parameter definitions, formatting and parsing
engine-sf2/         Rust: polyphonic multi-channel synth around sfl2, plus reverb and chorus
vendor/sfl2/        SFLT's SoundFont engine (GPL-3.0)
third_party/        CLAP SDK, sfizz
```

Files load on a background thread. The new engine is handed to the audio thread lock-free, and the old one is freed on the main thread, so loading never glitches playback.

Changes made to the SFLT core (`vendor/sfl2`):
- added the SoundFont 2.01 default modulators for channel pressure, CC91 (reverb) and CC93 (chorus)
- made channel and poly pressure real modulation sources
- made sample reads, and instrument and sample lookups, bounds-checked, so a damaged file cannot crash REAPER

Additions around the core that SFLT didn't have: sustain and sostenuto pedals, per-channel GM programs, RPNs, a working reverb and chorus, and a sustained note that is struck again releasing its previous voice.

## Tests

```sh
# Rust engine against a real SoundFont
SF_TEST_FONT=/path/to/font.sf3 cargo test --release --manifest-path engine-sf2/Cargo.toml -- --nocapture

# the built plug-in in a headless CLAP host (writes a WAV)
build/sfz123_test "build/sfz123.clap" /path/to/instrument.sfz /tmp/out.wav

# the editor: snapshot plus accessibility tree (optional tab index)
build/sfz123_gui_test "build/sfz123.clap" /path/to/instrument.sf2 /tmp/gui.png 1
```

## Known limitations

- An SFZ instrument is rebuilt when its polyphony changes, because sfizz can't change its voice count while running.
- The SoundFont envelope, filter, vibrato and effect settings don't apply to SFZ instruments. SFZ files define their own.
- sfizz doesn't support a few SFZ opcodes (Splendid Grand's `ampeg_release_shape` is one). The status line lists any it skipped.
- macOS only (the editor is AppKit).

## Credits and license

- SoundFont engine from [SFLT](https://estrobiologist.gumroad.com/l/sflt) by ash taylor, GPL-3.0
- SFZ engine: [sfizz](https://github.com/sfztools/sfizz), BSD-2-Clause
- [CLAP SDK](https://github.com/free-audio/clap), MIT

sfz123 is GPL-3.0-or-later (see `LICENSE`).
