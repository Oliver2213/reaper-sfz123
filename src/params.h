// Parameter definitions shared by the CLAP layer, the engines and the GUI.
#pragma once

#include <clap/clap.h>

#include <optional>
#include <string>
#include <vector>

namespace sfp {

enum ParamId : clap_id {
    // Main
    kVolume = 0,
    kTranspose = 1,
    kTuning = 2,
    kVelocityCurve = 3,
    kPolyphony = 4,
    kQuality = 5,

    // SF2/SF3 playback
    kPreset = 10,
    kMidiMode = 11,
    kFollowProgramChange = 12,
    kBendRange = 13,
    kMono = 14,
    kGlide = 15,

    // SF2/SF3 envelope, filter and vibrato overrides
    kAttack = 20,
    kDecay = 21,
    kSustain = 22,
    kRelease = 23,
    kFilterCutoff = 24,
    kFilterEnabled = 25,
    kVibratoDepth = 26,
    kVibratoRate = 27,
    kVibratoDelay = 28,

    // SF2/SF3 effects
    kReverbLevel = 30,
    kReverbRoom = 31,
    kReverbDamping = 32,
    kReverbWidth = 33,
    kChorusLevel = 34,
    kChorusRate = 35,
    kChorusDepth = 36,

    // Instrument controls (MIDI CCs, named after the loaded instrument)
    kControlFirst = 100,
};

constexpr int kNumControls = 16;

enum class Kind { Continuous, Integer, Enum, Toggle };
enum class Group { Main, Sf2Playback, Sf2Shaping, Sf2Effects, Controls };

struct ParamDef {
    clap_id id;
    const char* name;
    Group group;
    Kind kind;
    double min, max, def;
    double step;          // increment for keyboard / VoiceOver adjustment
    const char* help;     // tooltip and accessibility help
    std::vector<const char*> choices; // for Enum
    bool sf2Only = false;
};

const std::vector<ParamDef>& paramDefs();
const ParamDef* findParam(clap_id id);
int paramIndex(clap_id id); // index into paramDefs(), or -1

const char* groupTitle(Group g);
const char* groupModule(Group g); // CLAP module path

// Envelope-time parameters are stored as 0..1 positions; 0 means "instrument default".
double timeParamToMs(double v);
double msToTimeParam(double ms);

// Formatting with full, screen-reader friendly units ("-6.0 dB", "Instrument default").
std::string formatParam(const ParamDef& def, double value);
std::optional<double> parseParam(const ParamDef& def, const std::string& text);

// Note name with middle C (60) as C4, e.g. "F♯3". The sharp sign is read as "sharp" by VoiceOver.
std::string noteName(int key);

// "A0 to C8", or "all keys"
std::string keyRangeText(int lo, int hi);

// Velocity curves (applied before the engines).
float applyVelocityCurve(int curve, float velocity01);

} // namespace sfp
