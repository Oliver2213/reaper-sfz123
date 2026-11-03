// Common interface for the SF2/SF3 (Rust sfl2) and SFZ (sfizz) engines.
#pragma once

#include "sf2engine.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sfp {

enum class InstrumentKind { None, Sf2, Sf3, Sfz };

struct PresetInfo {
    uint16_t bank = 0;
    uint16_t program = 0;
    std::string name;
    uint8_t keyLo = 0, keyHi = 127; // keys that produce sound
};

struct ControlInfo {
    int cc = 0;
    std::string label;
    float defaultValue = 0; // 0..127
};

struct KeyName {
    int key = 0;
    std::string name;
};

// Immutable description of a loaded instrument (built on the loader thread).
struct InstrumentInfo {
    InstrumentKind kind = InstrumentKind::None;
    std::string path;
    std::string title;           // instrument name for display
    std::vector<PresetInfo> presets;
    std::vector<ControlInfo> controls;
    std::vector<KeyName> keyNames;  // SFZ key labels and keyswitches
    std::string summary;         // e.g. "SFZ instrument, 150 regions"
    std::vector<std::string> warnings;
};

const char* kindName(InstrumentKind k);
InstrumentKind kindForPath(const std::string& path);

// Values the plugin pushes into the engine on the audio thread.
struct EngineSettings {
    Sf2Settings sf2 {};
    int sfzQuality = 2;
    float tuningCents = 0;
    int polyphony = 128;
};

class Engine {
public:
    virtual ~Engine() = default;

    // Control thread, while the engine is not being processed.
    virtual void prepare(double sampleRate, uint32_t maxFrames) = 0;
    double sampleRate() const { return sampleRate_; }

    // Real-time thread.
    virtual void applySettings(const EngineSettings& s) = 0;
    virtual void noteOn(int channel, int key, float velocity) = 0;
    virtual void noteOff(int channel, int key, float velocity) = 0;
    virtual void choke(int channel, int key) = 0;
    virtual void controlChange(int channel, int cc, float value127) = 0;
    // returns true if the plugin's Preset parameter should follow (SF2 single mode)
    virtual bool programChange(int channel, int program) = 0;
    virtual void pitchBend(int channel, int value14) = 0;
    virtual void channelPressure(int channel, float value01) = 0;
    virtual void polyPressure(int channel, int key, float value01) = 0;
    virtual void allSoundOff() = 0;
    virtual void resetControllers() = 0;
    virtual void render(float* left, float* right, uint32_t frames) = 0;
    virtual int activeVoices() = 0;
    virtual int selectedPreset() { return -1; }
    virtual void setFreewheel(bool) {}
    virtual void transport(double bpm, int beatsPerBar, int beatUnit, bool playing, int bar, double barBeat) {}

    // Polyphony that requires rebuilding the engine (SFZ); -1 if changes apply live.
    virtual int fixedPolyphony() const { return -1; }

    const InstrumentInfo& info() const { return info_; }

protected:
    InstrumentInfo info_;
    double sampleRate_ = 44100;
};

struct LoadRequest {
    std::string path;
    double sampleRate = 44100;
    uint32_t maxFrames = 1024;
    int polyphony = 128;
};

// Loader-thread entry points. On failure return null and fill `error`.
// `progress` receives 0..1000 while loading when the format can report it.
std::unique_ptr<Engine> loadSf2Engine(const LoadRequest& req, std::atomic<int>& progress, std::string& error);
std::unique_ptr<Engine> loadSfzEngine(const LoadRequest& req, std::atomic<int>& progress, std::string& error);

} // namespace sfp
