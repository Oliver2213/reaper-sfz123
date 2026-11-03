// SF2/SF3 engine: thin C++ wrapper over the Rust sfl2-based synth.
#include "engine.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

namespace sfp {

namespace {

uint8_t to7(float v127)
{
    return (uint8_t)std::clamp((int)std::lround(v127), 0, 127);
}

class Sf2Engine final : public Engine {
public:
    Sf2Engine(Sf2Font* font, Sf2Synth* synth) : font_(font), synth_(synth) {}

    ~Sf2Engine() override
    {
        sf2_synth_free(synth_);
        sf2_font_free(font_);
    }

    InstrumentInfo& mutableInfo() { return info_; }

    void prepare(double sampleRate, uint32_t) override
    {
        sampleRate_ = sampleRate;
        sf2_synth_set_sample_rate(synth_, (float)sampleRate);
    }

    void applySettings(const EngineSettings& s) override
    {
        if (std::memcmp(&s.sf2, &last_, sizeof(Sf2Settings)) == 0)
            return;
        last_ = s.sf2;
        sf2_synth_apply_settings(synth_, &s.sf2);
    }

    void noteOn(int ch, int key, float vel) override
    {
        int v = std::clamp((int)std::lround(vel * 127.0f), 1, 127);
        sf2_synth_note_on(synth_, (uint8_t)ch, (uint8_t)key, (uint8_t)v);
    }

    void noteOff(int ch, int key, float) override { sf2_synth_note_off(synth_, (uint8_t)ch, (uint8_t)key); }
    void choke(int ch, int key) override { sf2_synth_choke(synth_, (uint8_t)ch, (uint8_t)key); }
    void controlChange(int ch, int cc, float v) override { sf2_synth_control_change(synth_, (uint8_t)ch, (uint8_t)cc, to7(v)); }
    bool programChange(int ch, int program) override { return sf2_synth_program_change(synth_, (uint8_t)ch, (uint8_t)program); }
    void pitchBend(int ch, int v14) override { sf2_synth_pitch_bend(synth_, (uint8_t)ch, (uint16_t)std::clamp(v14, 0, 16383)); }
    void channelPressure(int ch, float v) override { sf2_synth_channel_pressure(synth_, (uint8_t)ch, to7(v * 127.0f)); }
    void polyPressure(int ch, int key, float v) override { sf2_synth_poly_pressure(synth_, (uint8_t)ch, (uint8_t)key, to7(v * 127.0f)); }
    void allSoundOff() override { sf2_synth_all_sound_off(synth_); }
    void resetControllers() override { sf2_synth_reset(synth_); }
    void render(float* l, float* r, uint32_t n) override { sf2_synth_render(synth_, l, r, n); }
    int activeVoices() override { return (int)sf2_synth_active_voices(synth_); }
    int selectedPreset() override { return sf2_synth_selected_preset(synth_); }

private:
    Sf2Font* font_;
    Sf2Synth* synth_;
    Sf2Settings last_ {};
};

} // namespace

std::unique_ptr<Engine> loadSf2Engine(const LoadRequest& req, std::atomic<int>& progress, std::string& error)
{
    Sf2Progress* prog = sf2_progress_new();
    std::atomic<bool> done { false };

    // forward the Rust progress counter while the blocking load runs
    std::thread poller([&] {
        while (!done.load()) {
            uint32_t phase = 0;
            uint32_t p = sf2_progress_get(prog, &phase);
            // reading is the first half for SF3 files, decoding the second
            progress.store(phase == 2 ? 500 + (int)p / 2 : (int)p);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });

    char err[512] = { 0 };
    Sf2Font* font = sf2_font_load(req.path.c_str(), prog, err, sizeof(err));
    done.store(true);
    poller.join();
    sf2_progress_free(prog);

    if (!font) {
        error = err[0] ? err : "the file could not be read";
        return nullptr;
    }

    Sf2Synth* synth = sf2_synth_new(font, (float)req.sampleRate);
    auto engine = std::make_unique<Sf2Engine>(font, synth);
    engine->prepare(req.sampleRate, req.maxFrames);

    InstrumentInfo& info = engine->mutableInfo();
    info.path = req.path;
    info.kind = kindForPath(req.path);

    char name[256] = { 0 };
    sf2_font_name(font, name, sizeof(name));
    info.title = name;

    uint32_t n = sf2_font_preset_count(font);
    for (uint32_t i = 0; i < n; ++i) {
        PresetInfo p;
        char pname[64] = { 0 };
        sf2_font_preset(font, i, &p.bank, &p.program, pname, sizeof(pname));
        sf2_font_preset_keys(font, i, &p.keyLo, &p.keyHi);
        p.name = pname[0] ? pname : "Untitled";
        info.presets.push_back(std::move(p));
    }

    uint32_t samples = 0, instruments = 0;
    sf2_font_counts(font, &samples, &instruments);

    info.summary = std::string(kindName(info.kind)) + " SoundFont, " + std::to_string(n) + (n == 1 ? " preset, " : " presets, ")
        + std::to_string(instruments) + " instruments, " + std::to_string(samples) + " samples";
    if (uint32_t silent = sf2_font_silent_presets(font))
        info.summary += ". " + std::to_string(silent) + (silent == 1 ? " silent preset was" : " silent presets were") + " hidden";
    if (sf2_font_all_silent(font))
        info.warnings.push_back("no preset appears to reach an audible sample, so all are listed");

    // The standard SoundFont controllers, so they are reachable without MIDI.
    info.controls = {
        { 1, "Modulation wheel", 0 },
        { 11, "Expression", 127 },
        { 64, "Sustain pedal", 0 },
        { 91, "Reverb send", 0 },
        { 93, "Chorus send", 0 },
    };

    progress.store(1000);
    return engine;
}

} // namespace sfp
