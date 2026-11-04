// The CLAP plugin: owns the engines, parameters, state, and the loader thread.
#pragma once

#include "engine.h"
#include "params.h"
#include "reaper.h"

#include <clap/clap.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace sfp {

class Gui;

extern const clap_plugin_descriptor kDescriptor;

// Single-producer single-consumer queue of GUI parameter gestures, forwarded to the host.
struct UiParamEvent {
    enum Type : uint8_t { Begin, Value, End } type;
    clap_id id;
    double value;
};

class UiEventQueue {
public:
    bool push(const UiParamEvent& e)
    {
        size_t w = write_.load(std::memory_order_relaxed);
        size_t next = (w + 1) % kSize;
        if (next == read_.load(std::memory_order_acquire))
            return false;
        buf_[w] = e;
        write_.store(next, std::memory_order_release);
        return true;
    }

    bool pop(UiParamEvent& e)
    {
        size_t r = read_.load(std::memory_order_relaxed);
        if (r == write_.load(std::memory_order_acquire))
            return false;
        e = buf_[r];
        read_.store((r + 1) % kSize, std::memory_order_release);
        return true;
    }

private:
    static constexpr size_t kSize = 1024;
    std::array<UiParamEvent, kSize> buf_ {};
    std::atomic<size_t> write_ { 0 }, read_ { 0 };
};

enum class LoadState { Empty, Loading, Ready, Error };

class Plugin {
public:
    explicit Plugin(const clap_host* host);
    ~Plugin();

    const clap_plugin* clapPlugin() const { return &plugin_; }
    static Plugin* from(const clap_plugin* p) { return static_cast<Plugin*>(p->plugin_data); }

    // CLAP core
    bool init();
    bool activate(double sampleRate, uint32_t minFrames, uint32_t maxFrames);
    void deactivate();
    void reset();
    clap_process_status process(const clap_process* p);
    const void* extension(const char* id);
    void onMainThread();

    // clap.params
    uint32_t paramsCount() const;
    bool paramsInfo(uint32_t index, clap_param_info* info) const;
    bool paramsValue(clap_id id, double* value) const;
    bool paramsValueToText(clap_id id, double value, char* out, uint32_t size) const;
    bool paramsTextToValue(clap_id id, const char* text, double* value) const;
    void paramsFlush(const clap_input_events* in, const clap_output_events* out);

    // clap.state
    bool stateSave(const clap_ostream* stream, bool projectRelative = true);
    bool stateLoad(const clap_istream* stream);

    // clap.note-name
    uint32_t noteNameCount() const;
    bool noteNameGet(uint32_t index, clap_note_name* name) const;

    // clap.timer-support
    void onTimer(clap_id id);

    // clap.render
    void setOffline(bool offline) { offline_.store(offline); }

    // clap.gui
    bool guiCreate();
    void guiDestroy();
    bool guiSetParent(const clap_window* window);
    bool guiShow();
    bool guiHide();
    void guiGetSize(uint32_t* w, uint32_t* h) const;

    // API used by the GUI (main thread)
    const InstrumentInfo& instrument() const { return info_; }
    LoadState loadState() const { return loadState_; }
    const std::string& statusMessage() const { return statusMessage_; }
    int loadProgress() const { return loadProgress_.load(); }
    const std::string& requestedPath() const { return requestedPath_; }
    uint64_t revision() const { return revision_.load(); }
    int activeVoices() const { return activeVoices_.load(); }
    std::string projectLocationText() const; // where the instrument lives relative to the project
    bool inReaper() const { return reaper_.available(); }
    void updateFxName(); // names the FX and its track in REAPER after the instrument, unless the user renamed them

    void loadFile(const std::string& path); // from the user: resets preset and controls
    void reloadFile();
    void unloadFile();

    double param(clap_id id) const;
    std::string paramDisplayName(clap_id id) const;
    std::string formatValue(clap_id id, double value) const;
    bool paramRelevant(clap_id id) const; // false for SF2-only params with an SFZ, and unused controls
    void uiBeginGesture(clap_id id);
    void uiSetParam(clap_id id, double value);
    void uiEndGesture(clap_id id);

    const clap_host* host() const { return host_; }

private:
    struct LoadJob {
        uint64_t generation = 0;
        LoadRequest request;
        bool applyDefaults = false;
        std::unique_ptr<Engine> engine;
        std::string error;
    };

    void startLoad(const std::string& path, bool applyDefaults);
    // saved path -> file on disk: relative to the project folder, else REAPER's media search
    std::string locateInstrument(const std::string& saved, const std::string& projectDir) const;
    void resolvePendingPath();
    void instrumentRenamed(const char* path);
    void registerProjectFiles();
    bool reloadAfterCopy_ = false;
    void loaderMain();
    void finishLoad(std::unique_ptr<LoadJob> job);
    void publishEngine(Engine* e);
    void collectRetired();
    void setStatus(LoadState s, std::string message);
    void bumpRevision();
    void logMessage(clap_log_severity sev, const std::string& msg) const;

    // audio thread helpers
    void swapPendingEngine();
    void handleEvent(const clap_event_header* h, const clap_output_events* out);
    void handleMidi(const uint8_t* data, const clap_output_events* out);
    void handleParamValue(clap_id id, double value);
    void controlFromMidi(int cc, float value, const clap_output_events* out);
    void sendUiEvents(const clap_output_events* out);
    void syncControls();
    void buildSettings(EngineSettings& s) const;
    void renderChunk(float* l, float* r, uint32_t frames);
    void noteOn(int channel, int key, float velocity);
    void noteOff(int channel, int key, float velocity);
    void emitParam(const clap_output_events* out, clap_id id, double value);

    void setParamValue(clap_id id, double v);

    const clap_host* host_;
    const clap_host_params* hostParams_ = nullptr;
    const clap_host_state* hostState_ = nullptr;
    const clap_host_log* hostLog_ = nullptr;
    const clap_host_note_name* hostNoteName_ = nullptr;
    const clap_host_timer_support* hostTimer_ = nullptr;
    ReaperApi reaper_;
    clap_plugin plugin_;

    std::unique_ptr<std::atomic<double>[]> values_;

    // engines: current_ is owned by the audio thread while active
    Engine* current_ = nullptr;
    std::atomic<Engine*> pending_ { nullptr };
    static constexpr int kRetireSlots = 8;
    std::array<std::atomic<Engine*>, kRetireSlots> retired_ {};
    std::atomic<bool> active_ { false };
    double sampleRate_ = 44100;
    uint32_t maxFrames_ = 1024;

    // audio-thread state
    std::array<std::array<int8_t, 128>, 16> noteMap_ {};
    std::array<float, kNumControls> lastSentControl_ {};
    std::atomic<bool> settingsDirty_ { true };
    EngineSettings settings_ {};
    float gain_ = 1.0f;
    bool engineFresh_ = false;
    std::atomic<bool> offline_ { false };
    std::atomic<int> activeVoices_ { 0 };
    std::atomic<bool> polyphonyReload_ { false };
    std::vector<float> scratchL_, scratchR_;

    UiEventQueue uiEvents_;

    // main-thread state
    InstrumentInfo info_;
    std::string requestedPath_;
    // controls whose value came from saved state (others take the instrument's default on load)
    std::array<bool, kNumControls> controlFromState_ {};
    int loadedPolyphony_ = -1;
    // preset saved as bank * 128 + program; preferred over the saved index, which shifts
    // if the preset list changes (e.g. silent presets being hidden)
    int pendingPresetBankProgram_ = -1;
    std::string unresolvedPath_; // project-relative path waiting for the project folder
    clap_id resolveTimer_ = CLAP_INVALID_ID;
    // FX and track names we gave the plug-in in REAPER, and what they were based on
    std::string fxName_, trackName_;
    clap_id nameTimer_ = CLAP_INVALID_ID;
    std::string fxNameBasis_;
    std::string fxNameForInstrument() const;
    void applyPendingPreset();
    // instrument control values saved by CC number, preferred over slot order
    std::map<int, double> pendingControlValues_;
    void applyPendingControls(); // > 0 when the loaded engine must be rebuilt to change polyphony
    LoadState loadState_ = LoadState::Empty;
    std::string statusMessage_;
    std::atomic<uint64_t> revision_ { 1 };
    Gui* gui_ = nullptr;

    // loader thread
    std::thread loader_;
    std::mutex loadMutex_;
    std::condition_variable loadCv_;
    std::unique_ptr<LoadJob> queuedJob_;
    std::unique_ptr<LoadJob> finishedJob_;
    bool quit_ = false;
    std::atomic<uint64_t> loadGeneration_ { 0 };
    std::atomic<int> loadProgress_ { 0 };
};

// Implemented in gui_mac.mm
class Gui {
public:
    static Gui* create(Plugin& plugin);
    virtual ~Gui() = default;
    virtual bool attach(void* nsview) = 0;
    virtual void setVisible(bool visible) = 0;
    static void preferredSize(uint32_t* w, uint32_t* h);
};

} // namespace sfp
