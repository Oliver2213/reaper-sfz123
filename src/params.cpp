#include "params.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sfp {

static std::vector<ParamDef> buildDefs()
{
    using K = Kind;
    using G = Group;
    std::vector<ParamDef> d = {
        { kVolume, "Volume", G::Main, K::Continuous, -60, 12, 0, 0.5,
          "Output level in decibels. The minimum silences the instrument." },
        { kTranspose, "Transpose", G::Main, K::Integer, -48, 48, 0, 1,
          "Shifts incoming notes by semitones." },
        { kTuning, "Fine tune", G::Main, K::Continuous, -100, 100, 0, 1,
          "Fine tuning in cents (hundredths of a semitone)." },
        { kVelocityCurve, "Velocity curve", G::Main, K::Enum, 0, 4, 0, 1,
          "How hard you need to play. Soft makes quiet playing louder, hard needs more force, fixed ignores velocity.",
          { "Linear", "Soft", "Extra soft", "Hard", "Fixed (full velocity)" } },
        { kPolyphony, "Polyphony", G::Main, K::Integer, 1, 256, 128, 8,
          "Maximum number of voices sounding at once. For SFZ instruments a change reloads the instrument." },
        { kQuality, "Interpolation quality", G::Main, K::Enum, 0, 3, 1, 1,
          "Sample playback quality. Higher settings sound cleaner when pitching samples and use more CPU.",
          { "Fast (linear)", "Standard", "High", "Best" } },

        { kPreset, "Preset", G::Sf2Playback, K::Integer, 0, 1023, 0, 1,
          "SoundFont preset to play. MIDI program changes can also select presets.", {}, true },
        { kMidiMode, "MIDI channel mode", G::Sf2Playback, K::Enum, 0, 1, 0, 1,
          "Single preset plays the chosen preset on every MIDI channel. General MIDI multitimbral gives each channel its own program, with drums on channel 10, for playing MIDI files.",
          { "Single preset on all channels", "General MIDI multitimbral" }, true },
        { kFollowProgramChange, "Follow program changes", G::Sf2Playback, K::Toggle, 0, 1, 1, 1,
          "In single preset mode, MIDI program change and bank select messages change the preset.", {}, true },
        { kBendRange, "Pitch bend range", G::Sf2Playback, K::Integer, 0, 48, 2, 1,
          "Pitch bend range in semitones. MIDI RPN 0 messages can also change it per channel.", {}, true },
        { kMono, "Mono legato", G::Sf2Playback, K::Toggle, 0, 1, 0, 1,
          "Plays one note at a time per channel, gliding between overlapping notes.", {}, true },
        { kGlide, "Glide time", G::Sf2Playback, K::Continuous, 0, 2000, 50, 10,
          "Time to glide between notes in mono legato mode, in milliseconds.", {}, true },

        { kAttack, "Attack override", G::Sf2Shaping, K::Continuous, 0, 1, 0, 0.01,
          "Replaces the SoundFont's attack time. At minimum the SoundFont's own value is used.", {}, true },
        { kDecay, "Decay override", G::Sf2Shaping, K::Continuous, 0, 1, 0, 0.01,
          "Replaces the SoundFont's decay time. At minimum the SoundFont's own value is used.", {}, true },
        { kSustain, "Sustain override", G::Sf2Shaping, K::Continuous, -1, 100, -1, 1,
          "Replaces the SoundFont's sustain level in percent. At minimum the SoundFont's own value is used.", {}, true },
        { kRelease, "Release override", G::Sf2Shaping, K::Continuous, 0, 1, 0, 0.01,
          "Replaces the SoundFont's release time. At minimum the SoundFont's own value is used.", {}, true },
        { kFilterCutoff, "Filter cutoff override", G::Sf2Shaping, K::Continuous, -1, 100, -1, 1,
          "Replaces the SoundFont's low-pass filter cutoff, in percent. At minimum the SoundFont's own value is used.", {}, true },
        { kFilterEnabled, "Low-pass filter", G::Sf2Shaping, K::Toggle, 0, 1, 1, 1,
          "Turn off to bypass the SoundFont's low-pass filters.", {}, true },
        { kVibratoDepth, "Vibrato depth override", G::Sf2Shaping, K::Continuous, -1, 200, -1, 5,
          "Replaces the SoundFont's vibrato depth in cents. At minimum the SoundFont's own value is used.", {}, true },
        { kVibratoRate, "Vibrato rate override", G::Sf2Shaping, K::Continuous, 0, 20, 0, 0.25,
          "Replaces the SoundFont's vibrato speed in hertz. At minimum the SoundFont's own value is used.", {}, true },
        { kVibratoDelay, "Vibrato delay override", G::Sf2Shaping, K::Continuous, -0.05, 5, -0.05, 0.05,
          "Replaces the time before vibrato starts, in seconds. At minimum the SoundFont's own value is used.", {}, true },

        { kReverbLevel, "Reverb level", G::Sf2Effects, K::Continuous, 0, 100, 40, 1,
          "Level of the built-in reverb. How much each sound uses it is set by the SoundFont and MIDI CC 91.", {}, true },
        { kReverbRoom, "Reverb room size", G::Sf2Effects, K::Continuous, 0, 100, 50, 1,
          "Size of the reverb room, in percent.", {}, true },
        { kReverbDamping, "Reverb damping", G::Sf2Effects, K::Continuous, 0, 100, 30, 1,
          "How quickly high frequencies die away in the reverb, in percent.", {}, true },
        { kReverbWidth, "Reverb width", G::Sf2Effects, K::Continuous, 0, 100, 100, 1,
          "Stereo width of the reverb, in percent.", {}, true },
        { kChorusLevel, "Chorus level", G::Sf2Effects, K::Continuous, 0, 100, 40, 1,
          "Level of the built-in chorus. How much each sound uses it is set by the SoundFont and MIDI CC 93.", {}, true },
        { kChorusRate, "Chorus rate", G::Sf2Effects, K::Continuous, 0.05, 5, 0.3, 0.05,
          "Chorus modulation speed in hertz.", {}, true },
        { kChorusDepth, "Chorus depth", G::Sf2Effects, K::Continuous, 0, 20, 8, 0.5,
          "Chorus modulation depth in milliseconds.", {}, true },
    };

    for (int i = 0; i < kNumControls; ++i) {
        d.push_back({ kControlFirst + (clap_id)i, "Control", G::Controls, K::Continuous, 0, 127, 0, 1,
                      "An instrument control, sent to the instrument as a MIDI controller." });
    }
    return d;
}

const std::vector<ParamDef>& paramDefs()
{
    static const std::vector<ParamDef> defs = buildDefs();
    return defs;
}

int paramIndex(clap_id id)
{
    const auto& d = paramDefs();
    for (size_t i = 0; i < d.size(); ++i)
        if (d[i].id == id)
            return (int)i;
    return -1;
}

const ParamDef* findParam(clap_id id)
{
    int i = paramIndex(id);
    return i < 0 ? nullptr : &paramDefs()[(size_t)i];
}

const char* groupTitle(Group g)
{
    switch (g) {
    case Group::Main: return "Sound";
    case Group::Sf2Playback: return "SoundFont playback";
    case Group::Sf2Shaping: return "SoundFont envelope and filter";
    case Group::Sf2Effects: return "SoundFont effects";
    case Group::Controls: return "Instrument controls";
    }
    return "";
}

const char* groupModule(Group g)
{
    switch (g) {
    case Group::Main: return "Sound";
    case Group::Sf2Playback: return "SF2/SF3/Playback";
    case Group::Sf2Shaping: return "SF2/SF3/Envelope and filter";
    case Group::Sf2Effects: return "SF2/SF3/Effects";
    case Group::Controls: return "Instrument controls";
    }
    return "";
}

double timeParamToMs(double v)
{
    v = std::clamp(v, 0.0, 1.0);
    return 10000.0 * v * v * v;
}

double msToTimeParam(double ms)
{
    return std::cbrt(std::clamp(ms, 0.0, 10000.0) / 10000.0);
}

static bool isTimeParam(clap_id id)
{
    return id == kAttack || id == kDecay || id == kRelease;
}

static std::string fmt(const char* f, double v)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), f, v);
    return buf;
}

std::string formatParam(const ParamDef& def, double v)
{
    const char* instr = "Instrument default";

    if (def.kind == Kind::Enum) {
        int i = (int)std::lround(v);
        if (i >= 0 && i < (int)def.choices.size())
            return def.choices[(size_t)i];
        return std::to_string(i);
    }
    if (def.kind == Kind::Toggle)
        return v >= 0.5 ? "On" : "Off";

    switch (def.id) {
    case kVolume:
        if (v <= def.min + 1e-9)
            return "Muted";
        return fmt("%.1f dB", v);
    case kTranspose: {
        int st = (int)std::lround(v);
        if (st == 0)
            return "0 semitones";
        return (st > 0 ? "+" : "") + std::to_string(st) + (std::abs(st) == 1 ? " semitone" : " semitones");
    }
    case kTuning:
        return fmt("%+.0f cents", v);
    case kPolyphony:
        return std::to_string((int)std::lround(v)) + " voices";
    case kBendRange: {
        int st = (int)std::lround(v);
        return std::to_string(st) + (st == 1 ? " semitone" : " semitones");
    }
    case kGlide:
        return fmt("%.0f ms", v);
    case kSustain:
    case kFilterCutoff:
        if (v < 0)
            return instr;
        return fmt("%.0f%%", v);
    case kVibratoDepth:
        if (v < 0)
            return instr;
        return fmt("%.0f cents", v);
    case kVibratoRate:
        if (v <= 0)
            return instr;
        return fmt("%.2f Hz", v);
    case kVibratoDelay:
        if (v < 0)
            return instr;
        return fmt("%.2f s", v);
    case kReverbLevel:
    case kReverbRoom:
    case kReverbDamping:
    case kReverbWidth:
    case kChorusLevel:
        return fmt("%.0f%%", v);
    case kChorusRate:
        return fmt("%.2f Hz", v);
    case kChorusDepth:
        return fmt("%.1f ms", v);
    default:
        break;
    }

    if (isTimeParam(def.id)) {
        if (v <= 0)
            return instr;
        double ms = timeParamToMs(v);
        if (ms < 10)
            return fmt("%.2f ms", ms);
        if (ms < 1000)
            return fmt("%.0f ms", ms);
        return fmt("%.2f s", ms / 1000.0);
    }

    if (def.id >= kControlFirst && def.id < kControlFirst + kNumControls)
        return std::to_string((int)std::lround(v));

    if (def.kind == Kind::Integer)
        return std::to_string((int)std::lround(v));
    return fmt("%.2f", v);
}

static std::string lower(std::string s)
{
    for (auto& c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

std::optional<double> parseParam(const ParamDef& def, const std::string& textIn)
{
    std::string text = lower(textIn);
    while (!text.empty() && std::isspace((unsigned char)text.front()))
        text.erase(text.begin());

    if (def.kind == Kind::Enum) {
        for (size_t i = 0; i < def.choices.size(); ++i)
            if (lower(def.choices[i]).rfind(text, 0) == 0 && !text.empty())
                return (double)i;
    }
    if (def.kind == Kind::Toggle) {
        if (text == "on" || text == "yes" || text == "true")
            return 1.0;
        if (text == "off" || text == "no" || text == "false")
            return 0.0;
    }
    if (text.rfind("instr", 0) == 0 || text == "default") {
        return def.min;
    }
    if (def.id == kVolume && (text == "muted" || text == "-inf"))
        return def.min;

    char* end = nullptr;
    double v = std::strtod(text.c_str(), &end);
    if (end == text.c_str())
        return std::nullopt;

    std::string unit = end;
    while (!unit.empty() && std::isspace((unsigned char)unit.front()))
        unit.erase(unit.begin());

    if (isTimeParam(def.id)) {
        if (unit.rfind("s", 0) == 0 && unit.rfind("ms", 0) != 0)
            v *= 1000.0;
        if (v <= 0)
            return msToTimeParam(0.001); // explicit zero-length, not "default"
        return msToTimeParam(v);
    }
    return std::clamp(v, def.min, def.max);
}

std::string noteName(int key)
{
    static const char* names[] = { "C", "C♯", "D", "D♯", "E", "F", "F♯", "G", "G♯", "A", "A♯", "B" };
    key = std::clamp(key, 0, 127);
    return std::string(names[key % 12]) + std::to_string(key / 12 - 1);
}

std::string keyRangeText(int lo, int hi)
{
    if (lo <= 0 && hi >= 127)
        return "all keys";
    if (lo == hi)
        return "only " + noteName(lo);
    return noteName(lo) + " to " + noteName(hi);
}

float applyVelocityCurve(int curve, float v)
{
    v = std::clamp(v, 0.0f, 1.0f);
    switch (curve) {
    case 1: return std::pow(v, 0.65f);
    case 2: return std::pow(v, 0.4f);
    case 3: return std::pow(v, 1.6f);
    case 4: return 1.0f;
    default: return v;
    }
}

} // namespace sfp
