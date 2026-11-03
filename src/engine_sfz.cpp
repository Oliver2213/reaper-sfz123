// SFZ engine: wraps sfizz.
#include "engine.h"

#include <sfizz.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace sfp {

namespace {

// sfizz quality levels for our four interpolation settings
int sfizzQuality(int q)
{
    static const int map[] = { 1, 2, 3, 10 };
    return map[std::clamp(q, 0, 3)];
}

class SfzEngine final : public Engine {
public:
    explicit SfzEngine(int polyphony) : polyphony_(polyphony)
    {
        synth_.setNumVoices(polyphony);
        synth_.setSampleQuality(sfz::Sfizz::ProcessFreewheeling, 10);
    }

    sfz::Sfizz& synth() { return synth_; }
    InstrumentInfo& mutableInfo() { return info_; }

    void prepare(double sampleRate, uint32_t maxFrames) override
    {
        sampleRate_ = sampleRate;
        synth_.setSampleRate((float)sampleRate);
        synth_.setSamplesPerBlock((int)std::max<uint32_t>(maxFrames, 16));
    }

    void applySettings(const EngineSettings& s) override
    {
        if (s.sfzQuality != quality_) {
            quality_ = s.sfzQuality;
            synth_.setSampleQuality(sfz::Sfizz::ProcessLive, sfizzQuality(quality_));
        }
        if (s.tuningCents != tuning_) {
            tuning_ = s.tuningCents;
            synth_.setTuningFrequency(440.0f * std::pow(2.0f, tuning_ / 1200.0f));
        }
    }

    // SFZ has a single MIDI channel space; all channels feed the instrument.
    void noteOn(int, int key, float vel) override { synth_.hdNoteOn(0, key, std::max(vel, 1.0f / 127.0f)); }
    void noteOff(int, int key, float vel) override { synth_.hdNoteOff(0, key, vel); }
    void choke(int, int key) override { synth_.hdNoteOff(0, key, 0.0f); }

    void controlChange(int, int cc, float v) override
    {
        if (cc == 120) {
            synth_.allSoundOff();
            return;
        }
        synth_.hdcc(0, cc, std::clamp(v / 127.0f, 0.0f, 1.0f));
    }

    bool programChange(int, int program) override
    {
        synth_.programChange(0, program);
        return false;
    }

    void pitchBend(int, int v14) override { synth_.pitchWheel(0, std::clamp(v14, 0, 16383) - 8192); }
    void channelPressure(int, float v) override { synth_.hdChannelAftertouch(0, v); }
    void polyPressure(int, int key, float v) override { synth_.hdPolyAftertouch(0, key, v); }
    void allSoundOff() override { synth_.allSoundOff(); }
    void resetControllers() override { synth_.allSoundOff(); }

    void render(float* l, float* r, uint32_t n) override
    {
        float* bufs[2] = { l, r };
        synth_.renderBlock(bufs, n, 1);
    }

    int activeVoices() override { return synth_.getNumActiveVoices(); }

    void setFreewheel(bool on) override
    {
        if (on == freewheel_)
            return;
        freewheel_ = on;
        if (on)
            synth_.enableFreeWheeling();
        else
            synth_.disableFreeWheeling();
    }

    void transport(double bpm, int beatsPerBar, int beatUnit, bool playing, int bar, double barBeat) override
    {
        if (bpm > 0 && bpm != bpm_) {
            bpm_ = bpm;
            synth_.bpmTempo(0, (float)bpm);
        }
        if (beatsPerBar > 0 && beatUnit > 0 && (beatsPerBar != tsNum_ || beatUnit != tsDen_)) {
            tsNum_ = beatsPerBar;
            tsDen_ = beatUnit;
            synth_.timeSignature(0, beatsPerBar, beatUnit);
        }
        synth_.timePosition(0, bar, barBeat);
        if (playing != playing_) {
            playing_ = playing;
            synth_.playbackState(0, playing ? 1 : 0);
        }
    }

    int fixedPolyphony() const override { return polyphony_; }

private:
    sfz::Sfizz synth_;
    int polyphony_;
    int quality_ = -1;
    float tuning_ = 0;
    bool freewheel_ = false;
    double bpm_ = 0;
    int tsNum_ = 0, tsDen_ = 0;
    bool playing_ = false;
};

// Collects replies to sfizz messaging queries made during loading.
struct QueryState {
    std::map<std::string, float> floats;
    std::map<std::string, std::string> strings;
    std::map<std::string, std::vector<uint8_t>> blobs;
};

void onReply(void* data, int, const char* path, const char* sig, const sfizz_arg_t* args)
{
    auto* q = static_cast<QueryState*>(data);
    if (!sig || !sig[0])
        return;
    switch (sig[0]) {
    case 'f': q->floats[path] = args[0].f; break;
    case 'd': q->floats[path] = (float)args[0].d; break;
    case 'i': q->floats[path] = (float)args[0].i; break;
    case 's': q->strings[path] = args[0].s ? args[0].s : ""; break;
    case 'b':
        if (args[0].b)
            q->blobs[path].assign(args[0].b->data, args[0].b->data + args[0].b->size);
        break;
    default: break;
    }
}

bool bitSet(const std::vector<uint8_t>& bits, int i)
{
    size_t byte = (size_t)i / 8;
    return byte < bits.size() && (bits[byte] & (1u << (i % 8)));
}

const char* standardCcName(int cc)
{
    switch (cc) {
    case 1: return "Modulation wheel";
    case 2: return "Breath";
    case 4: return "Foot controller";
    case 7: return "Volume";
    case 10: return "Pan";
    case 11: return "Expression";
    case 64: return "Sustain pedal";
    case 66: return "Sostenuto pedal";
    case 67: return "Soft pedal";
    default: return nullptr;
    }
}

const char* noteName(int key)
{
    static const char* names[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    return names[key % 12];
}

} // namespace

std::unique_ptr<Engine> loadSfzEngine(const LoadRequest& req, std::atomic<int>& progress, std::string& error)
{
    auto engine = std::make_unique<SfzEngine>(std::clamp(req.polyphony, 1, 256));
    engine->prepare(req.sampleRate, req.maxFrames);
    sfz::Sfizz& synth = engine->synth();

    progress.store(100);
    if (!synth.loadSfzFile(req.path)) {
        error = "sfizz could not parse the SFZ file";
        return nullptr;
    }
    progress.store(900);

    InstrumentInfo& info = engine->mutableInfo();
    info.kind = InstrumentKind::Sfz;
    info.path = req.path;

    // title: file name without extension
    std::string file = req.path.substr(req.path.find_last_of('/') + 1);
    info.title = file.substr(0, file.find_last_of('.'));

    int regions = synth.getNumRegions();
    if (regions == 0) {
        error = "the SFZ file contains no playable regions (are its samples missing?)";
        return nullptr;
    }

    // query defaults and slots through sfizz's messaging interface
    QueryState q;
    auto client = sfz::Sfizz::createClient(&q);
    sfz::Sfizz::setReceiveCallback(*client, &onReply);
    synth.sendMessage(*client, 0, "/cc/slots", "", nullptr);
    synth.sendMessage(*client, 0, "/sw/last/slots", "", nullptr);

    std::map<int, std::string> labels;
    for (const auto& [cc, label] : synth.getCCLabels())
        if (cc < 128)
            labels[cc] = label;

    // labelled CCs first; if the instrument labels none, list the CCs it uses
    std::vector<int> ccs;
    for (const auto& [cc, _] : labels)
        ccs.push_back(cc);
    if (ccs.empty()) {
        const auto& used = q.blobs["/cc/slots"];
        for (int cc = 0; cc < 128; ++cc)
            if (bitSet(used, cc) && cc != 0 && cc != 32 && !(cc >= 120))
                ccs.push_back(cc);
    }

    for (int cc : ccs) {
        std::string path = "/cc" + std::to_string(cc) + "/default";
        synth.sendMessage(*client, 0, path.c_str(), "", nullptr);
        ControlInfo c;
        c.cc = cc;
        auto it = labels.find(cc);
        if (it != labels.end() && !it->second.empty())
            c.label = it->second;
        else if (const char* stdName = standardCcName(cc))
            c.label = stdName;
        else
            c.label = "CC " + std::to_string(cc);
        auto d = q.floats.find(path);
        c.defaultValue = d != q.floats.end() ? std::clamp(d->second, 0.0f, 1.0f) * 127.0f : (cc == 7 ? 100.0f : cc == 10 ? 64.0f : cc == 11 ? 127.0f : 0.0f);
        info.controls.push_back(std::move(c));
    }

    // keyswitches, then key labels
    const auto& sw = q.blobs["/sw/last/slots"];
    std::map<int, std::string> keyNames;
    for (int key = 0; key < 128; ++key) {
        if (!bitSet(sw, key))
            continue;
        std::string path = "/sw/last/" + std::to_string(key) + "/label";
        synth.sendMessage(*client, 0, path.c_str(), "", nullptr);
        std::string label = q.strings[path];
        keyNames[key] = "Keyswitch: " + (label.empty() ? std::string(noteName(key)) + std::to_string(key / 12 - 1) : label);
    }
    for (const auto& [key, label] : synth.getKeyLabels())
        if (!keyNames.count(key) && !label.empty())
            keyNames[key] = label;
    for (const auto& [key, name] : keyNames)
        info.keyNames.push_back({ key, name });

    // labels are informational, so only report opcodes that affect the sound
    std::vector<std::string> unsupported;
    for (const auto& op : synth.getUnknownOpcodes())
        if (op.size() < 6 || op.compare(op.size() - 6, 6, "_label") != 0)
            unsupported.push_back(op);
    size_t unknown = unsupported.size();
    if (unknown > 0) {
        std::string list;
        size_t shown = 0;
        for (const auto& op : unsupported) {
            if (shown++ == 8) {
                list += ", ...";
                break;
            }
            list += (list.empty() ? "" : ", ") + op;
        }
        info.warnings.push_back(std::to_string(unknown) + (unknown == 1 ? " unsupported SFZ opcode ignored: " : " unsupported SFZ opcodes ignored: ") + list);
    }

    info.summary = "SFZ instrument, " + std::to_string(regions) + (regions == 1 ? " region" : " regions");
    if (!info.keyNames.empty())
        info.summary += ", " + std::to_string(info.keyNames.size()) + " named keys";

    progress.store(1000);
    return engine;
}

} // namespace sfp
