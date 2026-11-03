#include "plugin.h"

#include "recent.h"
#include "sfz_files.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

namespace sfp {

const char* kindName(InstrumentKind k)
{
    switch (k) {
    case InstrumentKind::Sf2: return "SF2";
    case InstrumentKind::Sf3: return "SF3";
    case InstrumentKind::Sfz: return "SFZ";
    default: return "No";
    }
}

static std::string lowerExt(const std::string& path)
{
    auto dot = path.find_last_of('.');
    if (dot == std::string::npos)
        return "";
    std::string e = path.substr(dot + 1);
    for (auto& c : e)
        c = (char)std::tolower((unsigned char)c);
    return e;
}

InstrumentKind kindForPath(const std::string& path)
{
    std::string e = lowerExt(path);
    if (e == "sf2")
        return InstrumentKind::Sf2;
    if (e == "sf3")
        return InstrumentKind::Sf3;
    if (e == "sfz")
        return InstrumentKind::Sfz;

    // unknown extension: sniff for a RIFF/sfbk header
    if (FILE* f = std::fopen(path.c_str(), "rb")) {
        char hdr[12] = { 0 };
        size_t n = std::fread(hdr, 1, sizeof(hdr), f);
        std::fclose(f);
        if (n == 12 && std::memcmp(hdr, "RIFF", 4) == 0 && std::memcmp(hdr + 8, "sfbk", 4) == 0)
            return InstrumentKind::Sf2;
        return InstrumentKind::Sfz;
    }
    return InstrumentKind::None;
}

static std::string fileName(const std::string& path)
{
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Placeholder engine used when no instrument is loaded, so the audio thread never sees null.
class EmptyEngine final : public Engine {
public:
    void prepare(double sr, uint32_t) override { sampleRate_ = sr; }
    void applySettings(const EngineSettings&) override {}
    void noteOn(int, int, float) override {}
    void noteOff(int, int, float) override {}
    void choke(int, int) override {}
    void controlChange(int, int, float) override {}
    bool programChange(int, int) override { return false; }
    void pitchBend(int, int) override {}
    void channelPressure(int, float) override {}
    void polyPressure(int, int, float) override {}
    void allSoundOff() override {}
    void resetControllers() override {}
    void render(float* l, float* r, uint32_t n) override
    {
        std::memset(l, 0, n * sizeof(float));
        std::memset(r, 0, n * sizeof(float));
    }
    int activeVoices() override { return 0; }
};

// --- CLAP trampolines

namespace {

bool cInit(const clap_plugin* p) { return Plugin::from(p)->init(); }
void cDestroy(const clap_plugin* p) { delete Plugin::from(p); }
bool cActivate(const clap_plugin* p, double sr, uint32_t mn, uint32_t mx) { return Plugin::from(p)->activate(sr, mn, mx); }
void cDeactivate(const clap_plugin* p) { Plugin::from(p)->deactivate(); }
bool cStartProcessing(const clap_plugin*) { return true; }
void cStopProcessing(const clap_plugin*) {}
void cReset(const clap_plugin* p) { Plugin::from(p)->reset(); }
clap_process_status cProcess(const clap_plugin* p, const clap_process* pr) { return Plugin::from(p)->process(pr); }
const void* cExtension(const clap_plugin* p, const char* id) { return Plugin::from(p)->extension(id); }
void cOnMainThread(const clap_plugin* p) { Plugin::from(p)->onMainThread(); }

uint32_t cParamsCount(const clap_plugin* p) { return Plugin::from(p)->paramsCount(); }
bool cParamsInfo(const clap_plugin* p, uint32_t i, clap_param_info* info) { return Plugin::from(p)->paramsInfo(i, info); }
bool cParamsValue(const clap_plugin* p, clap_id id, double* v) { return Plugin::from(p)->paramsValue(id, v); }
bool cParamsValueToText(const clap_plugin* p, clap_id id, double v, char* out, uint32_t n) { return Plugin::from(p)->paramsValueToText(id, v, out, n); }
bool cParamsTextToValue(const clap_plugin* p, clap_id id, const char* t, double* v) { return Plugin::from(p)->paramsTextToValue(id, t, v); }
void cParamsFlush(const clap_plugin* p, const clap_input_events* in, const clap_output_events* out) { Plugin::from(p)->paramsFlush(in, out); }

const clap_plugin_params kParamsExt = { cParamsCount, cParamsInfo, cParamsValue, cParamsValueToText, cParamsTextToValue, cParamsFlush };

bool cStateSave(const clap_plugin* p, const clap_ostream* s) { return Plugin::from(p)->stateSave(s); }
bool cStateLoad(const clap_plugin* p, const clap_istream* s) { return Plugin::from(p)->stateLoad(s); }
const clap_plugin_state kStateExt = { cStateSave, cStateLoad };

// presets keep absolute paths, since they are loaded into other projects
bool cStateSaveContext(const clap_plugin* p, const clap_ostream* s, uint32_t context)
{
    return Plugin::from(p)->stateSave(s, context != CLAP_STATE_CONTEXT_FOR_PRESET);
}
bool cStateLoadContext(const clap_plugin* p, const clap_istream* s, uint32_t) { return Plugin::from(p)->stateLoad(s); }
const clap_plugin_state_context kStateContextExt = { cStateSaveContext, cStateLoadContext };

bool cPresetLoad(const clap_plugin* p, uint32_t kind, const char* location, const char* key)
{
    return Plugin::from(p)->presetLoad(kind, location, key);
}
const clap_plugin_preset_load kPresetLoadExt = { cPresetLoad };

void cOnTimer(const clap_plugin* p, clap_id id) { Plugin::from(p)->onTimer(id); }
const clap_plugin_timer_support kTimerExt = { cOnTimer };

uint32_t cAudioPortsCount(const clap_plugin*, bool isInput) { return isInput ? 0 : 1; }
bool cAudioPortsGet(const clap_plugin*, uint32_t index, bool isInput, clap_audio_port_info* info)
{
    if (isInput || index != 0)
        return false;
    info->id = 0;
    std::snprintf(info->name, sizeof(info->name), "%s", "Stereo out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports kAudioPortsExt = { cAudioPortsCount, cAudioPortsGet };

uint32_t cNotePortsCount(const clap_plugin*, bool isInput) { return isInput ? 1 : 0; }
bool cNotePortsGet(const clap_plugin*, uint32_t index, bool isInput, clap_note_port_info* info)
{
    if (!isInput || index != 0)
        return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI;
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    std::snprintf(info->name, sizeof(info->name), "%s", "MIDI in");
    return true;
}
const clap_plugin_note_ports kNotePortsExt = { cNotePortsCount, cNotePortsGet };

uint32_t cNoteNameCount(const clap_plugin* p) { return Plugin::from(p)->noteNameCount(); }
bool cNoteNameGet(const clap_plugin* p, uint32_t i, clap_note_name* n) { return Plugin::from(p)->noteNameGet(i, n); }
const clap_plugin_note_name kNoteNameExt = { cNoteNameCount, cNoteNameGet };

bool cRenderHasHardRealtime(const clap_plugin*) { return false; }
bool cRenderSet(const clap_plugin* p, clap_plugin_render_mode mode)
{
    Plugin::from(p)->setOffline(mode == CLAP_RENDER_OFFLINE);
    return true;
}
const clap_plugin_render kRenderExt = { cRenderHasHardRealtime, cRenderSet };

bool cGuiIsApiSupported(const clap_plugin*, const char* api, bool floating)
{
    return !floating && std::strcmp(api, CLAP_WINDOW_API_COCOA) == 0;
}
bool cGuiGetPreferredApi(const clap_plugin*, const char** api, bool* floating)
{
    *api = CLAP_WINDOW_API_COCOA;
    *floating = false;
    return true;
}
bool cGuiCreate(const clap_plugin* p, const char* api, bool floating)
{
    if (floating || std::strcmp(api, CLAP_WINDOW_API_COCOA) != 0)
        return false;
    return Plugin::from(p)->guiCreate();
}
void cGuiDestroy(const clap_plugin* p) { Plugin::from(p)->guiDestroy(); }
bool cGuiSetScale(const clap_plugin*, double) { return false; }
bool cGuiGetSize(const clap_plugin* p, uint32_t* w, uint32_t* h)
{
    Plugin::from(p)->guiGetSize(w, h);
    return true;
}
bool cGuiCanResize(const clap_plugin*) { return false; }
bool cGuiGetResizeHints(const clap_plugin*, clap_gui_resize_hints*) { return false; }
bool cGuiAdjustSize(const clap_plugin* p, uint32_t* w, uint32_t* h)
{
    Plugin::from(p)->guiGetSize(w, h);
    return true;
}
bool cGuiSetSize(const clap_plugin*, uint32_t, uint32_t) { return true; }
bool cGuiSetParent(const clap_plugin* p, const clap_window* w) { return Plugin::from(p)->guiSetParent(w); }
bool cGuiSetTransient(const clap_plugin*, const clap_window*) { return false; }
void cGuiSuggestTitle(const clap_plugin*, const char*) {}
bool cGuiShow(const clap_plugin* p) { return Plugin::from(p)->guiShow(); }
bool cGuiHide(const clap_plugin* p) { return Plugin::from(p)->guiHide(); }
const clap_plugin_gui kGuiExt = { cGuiIsApiSupported, cGuiGetPreferredApi, cGuiCreate, cGuiDestroy, cGuiSetScale, cGuiGetSize,
    cGuiCanResize, cGuiGetResizeHints, cGuiAdjustSize, cGuiSetSize, cGuiSetParent, cGuiSetTransient, cGuiSuggestTitle, cGuiShow,
    cGuiHide };

// O(1) id -> index lookup for the audio thread
const std::array<int16_t, 256>& idTable()
{
    static const std::array<int16_t, 256> t = [] {
        std::array<int16_t, 256> a {};
        a.fill(-1);
        const auto& d = paramDefs();
        for (size_t i = 0; i < d.size(); ++i)
            a[d[i].id] = (int16_t)i;
        return a;
    }();
    return t;
}

inline int indexOf(clap_id id)
{
    return id < 256 ? idTable()[id] : -1;
}

} // namespace

const char* const kFeatures[] = { CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SAMPLER, CLAP_PLUGIN_FEATURE_STEREO, nullptr };

const clap_plugin_descriptor kDescriptor = {
    CLAP_VERSION_INIT,
    "com.sfplayer.sf-player", // stored in REAPER projects: never change
    "SF Player",
    "Blake Oliver",
    "https://github.com/Oliver2213/reaper-sfz123",
    "https://github.com/Oliver2213/reaper-sfz123#readme",
    "https://github.com/Oliver2213/reaper-sfz123/issues",
    SFPLAYER_VERSION,
    "Accessible SF2, SF3 and SFZ instrument player",
    kFeatures,
};

// --- lifecycle

Plugin::Plugin(const clap_host* host) : host_(host)
{
    plugin_.desc = &kDescriptor;
    plugin_.plugin_data = this;
    plugin_.init = cInit;
    plugin_.destroy = cDestroy;
    plugin_.activate = cActivate;
    plugin_.deactivate = cDeactivate;
    plugin_.start_processing = cStartProcessing;
    plugin_.stop_processing = cStopProcessing;
    plugin_.reset = cReset;
    plugin_.process = cProcess;
    plugin_.get_extension = cExtension;
    plugin_.on_main_thread = cOnMainThread;

    const auto& defs = paramDefs();
    values_.reset(new std::atomic<double>[defs.size()]);
    for (size_t i = 0; i < defs.size(); ++i)
        values_[i].store(defs[i].def);

    for (auto& ch : noteMap_)
        ch.fill(-1);
    lastSentControl_.fill(-1.0f);
    for (auto& r : retired_)
        r.store(nullptr);

    current_ = new EmptyEngine();
    setStatus(LoadState::Empty, "No instrument loaded. Choose Open to load an SF2, SF3 or SFZ file.");
}

Plugin::~Plugin()
{
    {
        std::lock_guard<std::mutex> lock(loadMutex_);
        quit_ = true;
    }
    loadCv_.notify_all();
    if (loader_.joinable())
        loader_.join();

    if (hostTimer_ && resolveTimer_ != CLAP_INVALID_ID)
        hostTimer_->unregister_timer(host_, resolveTimer_);
    guiDestroy();
    collectRetired();
    delete pending_.exchange(nullptr);
    delete current_;
}

bool Plugin::init()
{
    hostParams_ = static_cast<const clap_host_params*>(host_->get_extension(host_, CLAP_EXT_PARAMS));
    hostState_ = static_cast<const clap_host_state*>(host_->get_extension(host_, CLAP_EXT_STATE));
    hostLog_ = static_cast<const clap_host_log*>(host_->get_extension(host_, CLAP_EXT_LOG));
    hostNoteName_ = static_cast<const clap_host_note_name*>(host_->get_extension(host_, CLAP_EXT_NOTE_NAME));
    hostTimer_ = static_cast<const clap_host_timer_support*>(host_->get_extension(host_, CLAP_EXT_TIMER_SUPPORT));
    hostPresetLoad_ = static_cast<const clap_host_preset_load*>(host_->get_extension(host_, CLAP_EXT_PRESET_LOAD));
    if (!hostPresetLoad_)
        hostPresetLoad_ = static_cast<const clap_host_preset_load*>(host_->get_extension(host_, CLAP_EXT_PRESET_LOAD_COMPAT));
    hostContextMenu_ = static_cast<const clap_host_context_menu*>(host_->get_extension(host_, CLAP_EXT_CONTEXT_MENU));
    if (!hostContextMenu_)
        hostContextMenu_ = static_cast<const clap_host_context_menu*>(host_->get_extension(host_, CLAP_EXT_CONTEXT_MENU_COMPAT));
    reaper_.init(host_, [](void* self, const char* path) { static_cast<Plugin*>(self)->instrumentRenamed(path); }, this);
    loader_ = std::thread([this] { loaderMain(); });
    return true;
}

bool Plugin::activate(double sampleRate, uint32_t, uint32_t maxFrames)
{
    sampleRate_ = sampleRate;
    maxFrames_ = std::max<uint32_t>(maxFrames, 32);

    if (Engine* p = pending_.exchange(nullptr)) {
        delete current_;
        current_ = p;
    }
    current_->prepare(sampleRate_, maxFrames_);

    scratchL_.assign(maxFrames_, 0.0f);
    scratchR_.assign(maxFrames_, 0.0f);
    engineFresh_ = true;
    settingsDirty_ = true;
    active_.store(true);
    resolvePendingPath();
    return true;
}

void Plugin::deactivate()
{
    active_.store(false);
    if (Engine* p = pending_.exchange(nullptr)) {
        delete current_;
        current_ = p;
        engineFresh_ = true;
    }
    collectRetired();
}

void Plugin::reset()
{
    current_->allSoundOff();
    for (auto& ch : noteMap_)
        ch.fill(-1);
}

const void* Plugin::extension(const char* id)
{
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &kParamsExt;
    if (!std::strcmp(id, CLAP_EXT_STATE))
        return &kStateExt;
    if (!std::strcmp(id, CLAP_EXT_STATE_CONTEXT))
        return &kStateContextExt;
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT))
        return &kTimerExt;
    if (!std::strcmp(id, CLAP_EXT_PRESET_LOAD) || !std::strcmp(id, CLAP_EXT_PRESET_LOAD_COMPAT))
        return &kPresetLoadExt;
    if (!std::strcmp(id, CLAP_EXT_AUDIO_PORTS))
        return &kAudioPortsExt;
    if (!std::strcmp(id, CLAP_EXT_NOTE_PORTS))
        return &kNotePortsExt;
    if (!std::strcmp(id, CLAP_EXT_NOTE_NAME))
        return &kNoteNameExt;
    if (!std::strcmp(id, CLAP_EXT_RENDER))
        return &kRenderExt;
    if (!std::strcmp(id, CLAP_EXT_GUI))
        return &kGuiExt;
    return nullptr;
}

void Plugin::logMessage(clap_log_severity sev, const std::string& msg) const
{
    if (hostLog_)
        hostLog_->log(host_, sev, msg.c_str());
}

// --- parameters

void Plugin::setParamValue(clap_id id, double v)
{
    int i = indexOf(id);
    if (i < 0)
        return;
    const ParamDef& d = paramDefs()[(size_t)i];
    if (!std::isfinite(v))
        return;
    v = std::clamp(v, d.min, d.max);
    if (d.kind != Kind::Continuous)
        v = std::round(v);
    values_[(size_t)i].store(v);
}

double Plugin::param(clap_id id) const
{
    int i = indexOf(id);
    return i < 0 ? 0.0 : values_[(size_t)i].load();
}

uint32_t Plugin::paramsCount() const
{
    return (uint32_t)paramDefs().size();
}

bool Plugin::paramRelevant(clap_id id) const
{
    const ParamDef* d = findParam(id);
    if (!d)
        return false;
    if (d->sf2Only && info_.kind == InstrumentKind::Sfz)
        return false;
    if (id >= kControlFirst && id < kControlFirst + kNumControls)
        return (size_t)(id - kControlFirst) < info_.controls.size();
    return true;
}

std::string Plugin::paramDisplayName(clap_id id) const
{
    if (id >= kControlFirst && id < kControlFirst + kNumControls) {
        size_t i = id - kControlFirst;
        if (i < info_.controls.size()) {
            const ControlInfo& c = info_.controls[i];
            return c.label + " (CC " + std::to_string(c.cc) + ")";
        }
        return "Control " + std::to_string(i + 1) + " (unused)";
    }
    const ParamDef* d = findParam(id);
    return d ? d->name : "";
}

std::string Plugin::formatValue(clap_id id, double v) const
{
    const ParamDef* d = findParam(id);
    if (!d)
        return "";
    if (id == kPreset) {
        if (info_.presets.empty())
            return "No preset";
        size_t i = (size_t)std::clamp((long)std::lround(v), 0L, (long)info_.presets.size() - 1);
        const PresetInfo& p = info_.presets[i];
        return p.name + " (bank " + std::to_string(p.bank) + ", program " + std::to_string(p.program) + ", "
            + keyRangeText(p.keyLo, p.keyHi) + ")";
    }
    return formatParam(*d, v);
}

bool Plugin::paramsInfo(uint32_t index, clap_param_info* info) const
{
    const auto& defs = paramDefs();
    if (index >= defs.size())
        return false;
    const ParamDef& d = defs[index];

    std::memset(info, 0, sizeof(*info));
    info->id = d.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    if (d.kind != Kind::Continuous)
        info->flags |= CLAP_PARAM_IS_STEPPED;
    if (d.kind == Kind::Enum || d.kind == Kind::Toggle)
        info->flags |= CLAP_PARAM_IS_ENUM;
    if (!paramRelevant(d.id) && info_.kind != InstrumentKind::None)
        info->flags |= CLAP_PARAM_IS_HIDDEN;
    if (d.id >= kControlFirst && (size_t)(d.id - kControlFirst) >= info_.controls.size())
        info->flags |= CLAP_PARAM_IS_HIDDEN;

    std::snprintf(info->name, sizeof(info->name), "%s", paramDisplayName(d.id).c_str());
    std::snprintf(info->module, sizeof(info->module), "%s", groupModule(d.group));
    info->min_value = d.min;
    info->max_value = d.max;
    info->default_value = d.def;
    if (d.id >= kControlFirst) {
        size_t i = d.id - kControlFirst;
        if (i < info_.controls.size())
            info->default_value = std::round(info_.controls[i].defaultValue);
    }
    return true;
}

bool Plugin::paramsValue(clap_id id, double* value) const
{
    int i = indexOf(id);
    if (i < 0)
        return false;
    *value = values_[(size_t)i].load();
    return true;
}

bool Plugin::paramsValueToText(clap_id id, double value, char* out, uint32_t size) const
{
    if (!findParam(id))
        return false;
    std::snprintf(out, size, "%s", formatValue(id, value).c_str());
    return true;
}

bool Plugin::paramsTextToValue(clap_id id, const char* text, double* value) const
{
    const ParamDef* d = findParam(id);
    if (!d || !text)
        return false;

    if (id == kPreset && !info_.presets.empty()) {
        std::string t = text;
        for (auto& c : t)
            c = (char)std::tolower((unsigned char)c);
        for (size_t i = 0; i < info_.presets.size(); ++i) {
            std::string n = info_.presets[i].name;
            for (auto& c : n)
                c = (char)std::tolower((unsigned char)c);
            if (!t.empty() && n.rfind(t, 0) == 0) {
                *value = (double)i;
                return true;
            }
        }
    }

    auto v = parseParam(*d, text);
    if (!v)
        return false;
    *value = *v;
    return true;
}

void Plugin::paramsFlush(const clap_input_events* in, const clap_output_events* out)
{
    uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE) {
            auto* ev = reinterpret_cast<const clap_event_param_value*>(h);
            handleParamValue(ev->param_id, ev->value);
        }
    }
    if (out)
        sendUiEvents(out);
}

void Plugin::handleParamValue(clap_id id, double value)
{
    setParamValue(id, value);
    settingsDirty_.store(true);
    if (id == kPolyphony) {
        polyphonyReload_.store(true);
        host_->request_callback(host_);
    }
}

void Plugin::uiBeginGesture(clap_id id)
{
    uiEvents_.push({ UiParamEvent::Begin, id, 0 });
}

void Plugin::uiSetParam(clap_id id, double value)
{
    setParamValue(id, value);
    settingsDirty_.store(true);
    uiEvents_.push({ UiParamEvent::Value, id, param(id) });
    if (hostParams_)
        hostParams_->request_flush(host_);
    if (id == kPolyphony) {
        polyphonyReload_.store(true);
        host_->request_callback(host_);
    }
}

void Plugin::uiEndGesture(clap_id id)
{
    uiEvents_.push({ UiParamEvent::End, id, 0 });
    if (hostParams_)
        hostParams_->request_flush(host_);
}

std::vector<HostMenuItem> Plugin::hostParamMenu(clap_id id) const
{
    std::vector<HostMenuItem> items;
    if (!hostContextMenu_)
        return items;
    auto add = [](const clap_context_menu_builder* b, clap_context_menu_item_kind_t kind, const void* data) {
        auto& out = *static_cast<std::vector<HostMenuItem>*>(b->ctx);
        HostMenuItem item;
        switch (kind) {
        case CLAP_CONTEXT_MENU_ITEM_ENTRY: {
            auto* e = static_cast<const clap_context_menu_entry*>(data);
            item = { HostMenuItem::Entry, e->label ? e->label : "", e->is_enabled, false, false, e->action_id };
            break;
        }
        case CLAP_CONTEXT_MENU_ITEM_CHECK_ENTRY: {
            auto* e = static_cast<const clap_context_menu_check_entry*>(data);
            item = { HostMenuItem::Entry, e->label ? e->label : "", e->is_enabled, true, e->is_checked, e->action_id };
            break;
        }
        case CLAP_CONTEXT_MENU_ITEM_SEPARATOR:
            item.kind = HostMenuItem::Separator;
            break;
        case CLAP_CONTEXT_MENU_ITEM_BEGIN_SUBMENU: {
            auto* e = static_cast<const clap_context_menu_submenu*>(data);
            item = { HostMenuItem::BeginSubmenu, e->label ? e->label : "", e->is_enabled };
            break;
        }
        case CLAP_CONTEXT_MENU_ITEM_END_SUBMENU:
            item.kind = HostMenuItem::EndSubmenu;
            break;
        case CLAP_CONTEXT_MENU_ITEM_TITLE: {
            auto* e = static_cast<const clap_context_menu_item_title*>(data);
            item = { HostMenuItem::Title, e->title ? e->title : "", e->is_enabled };
            break;
        }
        default:
            return false;
        }
        out.push_back(std::move(item));
        return true;
    };
    auto supports = [](const clap_context_menu_builder*, clap_context_menu_item_kind_t kind) {
        return kind <= CLAP_CONTEXT_MENU_ITEM_TITLE;
    };
    const clap_context_menu_builder builder { &items, add, supports };
    const clap_context_menu_target target { CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, id };
    if (!hostContextMenu_->populate(host_, &target, &builder))
        items.clear();
    return items;
}

void Plugin::performHostParamAction(clap_id id, clap_id action)
{
    const clap_context_menu_target target { CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, id };
    if (hostContextMenu_)
        hostContextMenu_->perform(host_, &target, action);
}

void Plugin::emitParam(const clap_output_events* out, clap_id id, double value)
{
    if (!out)
        return;
    clap_event_param_value ev {};
    ev.header.size = sizeof(ev);
    ev.header.time = 0;
    ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    ev.header.type = CLAP_EVENT_PARAM_VALUE;
    ev.header.flags = 0;
    ev.param_id = id;
    ev.cookie = nullptr;
    ev.note_id = -1;
    ev.port_index = -1;
    ev.channel = -1;
    ev.key = -1;
    ev.value = value;
    out->try_push(out, &ev.header);
}

void Plugin::sendUiEvents(const clap_output_events* out)
{
    UiParamEvent e;
    while (uiEvents_.pop(e)) {
        if (e.type == UiParamEvent::Value) {
            emitParam(out, e.id, e.value);
            continue;
        }
        clap_event_param_gesture g {};
        g.header.size = sizeof(g);
        g.header.time = 0;
        g.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        g.header.type = e.type == UiParamEvent::Begin ? CLAP_EVENT_PARAM_GESTURE_BEGIN : CLAP_EVENT_PARAM_GESTURE_END;
        g.header.flags = 0;
        g.param_id = e.id;
        out->try_push(out, &g.header);
    }
}

void Plugin::buildSettings(EngineSettings& s) const
{
    auto v = [this](clap_id id) { return param(id); };
    auto timeMs = [&](clap_id id) { double x = v(id); return x <= 0 ? -1.0f : (float)timeParamToMs(x); };

    Sf2Settings& f = s.sf2;
    f.polyphony = (uint32_t)v(kPolyphony);
    f.interpolation = (uint32_t)v(kQuality);
    f.multi_timbral = (uint32_t)v(kMidiMode);
    f.follow_program_change = (uint32_t)v(kFollowProgramChange);
    f.mono = (uint32_t)v(kMono);
    f.filter_enabled = (uint32_t)v(kFilterEnabled);
    f.selected_preset = (int32_t)v(kPreset);
    f.bend_range = (float)v(kBendRange);
    f.tuning_semitones = (float)(v(kTuning) / 100.0);
    f.glide_ms = (float)v(kGlide);
    f.attack_ms = timeMs(kAttack);
    f.decay_ms = timeMs(kDecay);
    f.release_ms = timeMs(kRelease);
    f.sustain_pct = v(kSustain) < 0 ? -1.0f : (float)v(kSustain);
    f.filter_pct = v(kFilterCutoff) < 0 ? -1.0f : (float)v(kFilterCutoff);
    f.vib_depth_cents = v(kVibratoDepth) < 0 ? -1.0f : (float)v(kVibratoDepth);
    f.vib_rate_hz = v(kVibratoRate) <= 0 ? -1.0f : (float)v(kVibratoRate);
    f.vib_delay_s = v(kVibratoDelay) < 0 ? -1.0f : (float)v(kVibratoDelay);
    f.reverb_level = (float)(v(kReverbLevel) / 100.0);
    f.reverb_room = (float)(v(kReverbRoom) / 100.0);
    f.reverb_damp = (float)(v(kReverbDamping) / 100.0);
    f.reverb_width = (float)(v(kReverbWidth) / 100.0);
    f.chorus_level = (float)(v(kChorusLevel) / 100.0);
    f.chorus_rate_hz = (float)v(kChorusRate);
    f.chorus_depth_ms = (float)v(kChorusDepth);

    s.sfzQuality = (int)v(kQuality);
    s.tuningCents = (float)v(kTuning);
    s.polyphony = (int)v(kPolyphony);
}

// --- audio

void Plugin::swapPendingEngine()
{
    Engine* p = pending_.exchange(nullptr, std::memory_order_acq_rel);
    if (!p)
        return;

    Engine* old = current_;
    bool placed = false;
    for (auto& slot : retired_) {
        Engine* expected = nullptr;
        if (slot.compare_exchange_strong(expected, old)) {
            placed = true;
            break;
        }
    }
    // If every retire slot is full (the main thread is stalled), the old engine is leaked
    // rather than freed on the audio thread.
    (void)placed;

    current_ = p;
    engineFresh_ = true;
    host_->request_callback(host_);
}

void Plugin::syncControls()
{
    const auto& controls = current_->info().controls;
    bool sfz = current_->info().kind == InstrumentKind::Sfz;
    size_t n = std::min<size_t>(controls.size(), kNumControls);
    for (size_t i = 0; i < n; ++i) {
        float v = (float)param(kControlFirst + (clap_id)i);
        if (v == lastSentControl_[i])
            continue;
        lastSentControl_[i] = v;
        if (sfz) {
            current_->controlChange(0, controls[i].cc, v);
        } else {
            for (int ch = 0; ch < 16; ++ch)
                current_->controlChange(ch, controls[i].cc, v);
        }
    }
}

void Plugin::controlFromMidi(int cc, float value, const clap_output_events* out)
{
    const auto& controls = current_->info().controls;
    size_t n = std::min<size_t>(controls.size(), kNumControls);
    for (size_t i = 0; i < n; ++i) {
        if (controls[i].cc != cc)
            continue;
        clap_id id = kControlFirst + (clap_id)i;
        setParamValue(id, value);
        lastSentControl_[i] = (float)param(id);
        emitParam(out, id, param(id));
    }
}

void Plugin::noteOn(int ch, int key, float vel)
{
    int t = key + (int)param(kTranspose);
    if (t < 0 || t > 127)
        return;
    noteMap_[(size_t)ch][(size_t)key] = (int8_t)t;
    float v = applyVelocityCurve((int)param(kVelocityCurve), vel);
    current_->noteOn(ch, t, std::max(v, 1.0f / 127.0f));
}

void Plugin::noteOff(int ch, int key, float vel)
{
    int t = noteMap_[(size_t)ch][(size_t)key];
    if (t < 0)
        return;
    noteMap_[(size_t)ch][(size_t)key] = -1;
    current_->noteOff(ch, t, vel);
}

void Plugin::handleMidi(const uint8_t* d, const clap_output_events* out)
{
    int status = d[0] & 0xF0;
    int ch = d[0] & 0x0F;
    int a = d[1] & 0x7F;
    int b = d[2] & 0x7F;

    switch (status) {
    case 0x90:
        if (b > 0) {
            noteOn(ch, a, b / 127.0f);
            break;
        }
        [[fallthrough]];
    case 0x80:
        noteOff(ch, a, b / 127.0f);
        break;
    case 0xA0: {
        int t = noteMap_[(size_t)ch][(size_t)a];
        current_->polyPressure(ch, t >= 0 ? t : a, b / 127.0f);
        break;
    }
    case 0xB0:
        if (a == 123 || a == 120) {
            // notes are tracked here too, so clear the map
            for (auto& k : noteMap_[(size_t)ch])
                k = -1;
        }
        current_->controlChange(ch, a, (float)b);
        controlFromMidi(a, (float)b, out);
        break;
    case 0xC0:
        if (current_->programChange(ch, a)) {
            int sel = current_->selectedPreset();
            if (sel >= 0) {
                setParamValue(kPreset, sel);
                emitParam(out, kPreset, sel);
            }
        }
        break;
    case 0xD0:
        current_->channelPressure(ch, a / 127.0f);
        break;
    case 0xE0:
        current_->pitchBend(ch, a | (b << 7));
        break;
    default:
        break;
    }
}

void Plugin::handleEvent(const clap_event_header* h, const clap_output_events* out)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;

    switch (h->type) {
    case CLAP_EVENT_NOTE_ON: {
        auto* ev = reinterpret_cast<const clap_event_note*>(h);
        if (ev->key < 0 || ev->key > 127)
            break;
        noteOn(ev->channel < 0 ? 0 : ev->channel & 15, ev->key, (float)ev->velocity);
        break;
    }
    case CLAP_EVENT_NOTE_OFF:
    case CLAP_EVENT_NOTE_CHOKE: {
        auto* ev = reinterpret_cast<const clap_event_note*>(h);
        for (int ch = 0; ch < 16; ++ch) {
            if (ev->channel >= 0 && ch != (ev->channel & 15))
                continue;
            for (int key = 0; key < 128; ++key) {
                if (ev->key >= 0 && key != ev->key)
                    continue;
                if (h->type == CLAP_EVENT_NOTE_OFF) {
                    noteOff(ch, key, (float)ev->velocity);
                } else {
                    int t = noteMap_[(size_t)ch][(size_t)key];
                    if (t >= 0) {
                        noteMap_[(size_t)ch][(size_t)key] = -1;
                        current_->choke(ch, t);
                    }
                }
            }
        }
        break;
    }
    case CLAP_EVENT_MIDI: {
        auto* ev = reinterpret_cast<const clap_event_midi*>(h);
        handleMidi(ev->data, out);
        break;
    }
    case CLAP_EVENT_MIDI_SYSEX: {
        auto* ev = reinterpret_cast<const clap_event_midi_sysex*>(h);
        static const uint8_t gmOn[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
        static const uint8_t gm2On[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x03, 0xF7 };
        static const uint8_t gsReset[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
        auto is = [&](const uint8_t* m, size_t n) { return ev->size >= n && std::memcmp(ev->buffer, m, n) == 0; };
        if (is(gmOn, sizeof(gmOn)) || is(gm2On, sizeof(gm2On)) || is(gsReset, sizeof(gsReset))) {
            current_->resetControllers();
            for (auto& c : noteMap_)
                c.fill(-1);
            lastSentControl_.fill(-1.0f);
        }
        break;
    }
    case CLAP_EVENT_PARAM_VALUE: {
        auto* ev = reinterpret_cast<const clap_event_param_value*>(h);
        handleParamValue(ev->param_id, ev->value);
        break;
    }
    default:
        break;
    }
}

void Plugin::renderChunk(float* l, float* r, uint32_t frames)
{
    if (settingsDirty_.exchange(false)) {
        buildSettings(settings_);
        current_->applySettings(settings_);
    }
    syncControls();
    current_->render(l, r, frames);
}

clap_process_status Plugin::process(const clap_process* p)
{
    swapPendingEngine();

    if (engineFresh_) {
        engineFresh_ = false;
        for (auto& ch : noteMap_)
            ch.fill(-1);
        lastSentControl_.fill(-1.0f);
        settingsDirty_.store(true);
    }

    const clap_input_events* in = p->in_events;
    const clap_output_events* out = p->out_events;

    sendUiEvents(out);

    current_->setFreewheel(offline_.load(std::memory_order_relaxed));

    if (const clap_event_transport* t = p->transport) {
        double bpm = (t->flags & CLAP_TRANSPORT_HAS_TEMPO) ? t->tempo : 0;
        int num = 0, den = 0;
        if (t->flags & CLAP_TRANSPORT_HAS_TIME_SIGNATURE) {
            num = t->tsig_num;
            den = t->tsig_denom;
        }
        double barBeat = 0;
        if (t->flags & CLAP_TRANSPORT_HAS_BEATS_TIMELINE)
            barBeat = (double)(t->song_pos_beats - t->bar_start) / (double)CLAP_BEATTIME_FACTOR;
        current_->transport(bpm, num, den, (t->flags & CLAP_TRANSPORT_IS_PLAYING) != 0, t->bar_number, std::max(0.0, barBeat));
    }

    const uint32_t frames = p->frames_count;
    float* outL = nullptr;
    float* outR = nullptr;
    if (p->audio_outputs_count > 0 && p->audio_outputs[0].data32) {
        outL = p->audio_outputs[0].data32[0];
        outR = p->audio_outputs[0].channel_count > 1 ? p->audio_outputs[0].data32[1] : nullptr;
    }

    uint32_t nev = in ? in->size(in) : 0;
    uint32_t ev = 0;
    uint32_t pos = 0;

    while (pos < frames) {
        while (ev < nev) {
            const clap_event_header* h = in->get(in, ev);
            if (h->time > pos)
                break;
            handleEvent(h, out);
            ++ev;
        }

        uint32_t next = frames;
        if (ev < nev)
            next = std::min(frames, in->get(in, ev)->time);
        next = std::min(next, pos + maxFrames_);
        if (next <= pos)
            next = pos + 1;

        uint32_t n = next - pos;
        if (outL) {
            float* r = outR ? outR + pos : scratchR_.data();
            renderChunk(outL + pos, r, n);
        } else {
            renderChunk(scratchL_.data(), scratchR_.data(), n);
        }
        pos = next;
    }
    // events stamped at or after the end of the block
    while (ev < nev)
        handleEvent(in->get(in, ev++), out);

    // master volume, smoothed to avoid zipper noise
    double db = param(kVolume);
    float target = db <= paramDefs()[(size_t)indexOf(kVolume)].min + 1e-9 ? 0.0f : (float)std::pow(10.0, db / 20.0);
    if (outL) {
        float step = (target - gain_) / (float)std::max<uint32_t>(frames, 1);
        for (uint32_t i = 0; i < frames; ++i) {
            gain_ += step;
            outL[i] *= gain_;
            if (outR)
                outR[i] *= gain_;
        }
        gain_ = target;
    }

    int voices = current_->activeVoices();
    activeVoices_.store(voices, std::memory_order_relaxed);

    if (current_->info().kind == InstrumentKind::None)
        return CLAP_PROCESS_SLEEP;
    return voices > 0 ? CLAP_PROCESS_CONTINUE : CLAP_PROCESS_CONTINUE_IF_NOT_QUIET;
}

// --- main thread: loading

void Plugin::bumpRevision()
{
    revision_.fetch_add(1);
}

void Plugin::setStatus(LoadState s, std::string message)
{
    loadState_ = s;
    statusMessage_ = std::move(message);
    bumpRevision();
}

void Plugin::loadFile(const std::string& path)
{
    unresolvedPath_.clear();
    startLoad(path, true);
}

void Plugin::reloadFile()
{
    if (!requestedPath_.empty())
        startLoad(requestedPath_, false);
}

void Plugin::unloadFile()
{
    loadGeneration_.fetch_add(1); // discard any load in flight
    requestedPath_.clear();
    unresolvedPath_.clear();
    reaper_.setProjectFiles({});
    loadedPolyphony_ = -1;
    publishEngine(new EmptyEngine());
    info_ = InstrumentInfo {};
    if (hostParams_)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_INFO);
    if (hostNoteName_)
        hostNoteName_->changed(host_);
    if (hostState_)
        hostState_->mark_dirty(host_);
    setStatus(LoadState::Empty, "No instrument loaded. Choose Open to load an SF2, SF3 or SFZ file.");
}

void Plugin::startLoad(const std::string& path, bool applyDefaults, std::optional<PresetOrigin> preset)
{
    auto job = std::make_unique<LoadJob>();
    job->generation = loadGeneration_.fetch_add(1) + 1;
    job->request.path = path;
    job->request.sampleRate = sampleRate_;
    job->request.maxFrames = maxFrames_;
    job->request.polyphony = (int)param(kPolyphony);
    job->applyDefaults = applyDefaults;
    job->preset = std::move(preset);

    requestedPath_ = path;
    loadProgress_.store(0);
    setStatus(LoadState::Loading, "Loading " + fileName(path) + "...");

    {
        std::lock_guard<std::mutex> lock(loadMutex_);
        queuedJob_ = std::move(job); // replaces a queued job that has not started yet
    }
    loadCv_.notify_one();
}

void Plugin::loaderMain()
{
    for (;;) {
        std::unique_ptr<LoadJob> job;
        {
            std::unique_lock<std::mutex> lock(loadMutex_);
            loadCv_.wait(lock, [this] { return quit_ || queuedJob_; });
            if (quit_)
                return;
            job = std::move(queuedJob_);
        }

        const std::string& path = job->request.path;
        struct stat st {};
        if (stat(path.c_str(), &st) != 0) {
            job->error = "the file was not found";
        } else if (S_ISDIR(st.st_mode)) {
            job->error = "it is a folder, not an instrument file";
        } else {
            InstrumentKind kind = kindForPath(path);
            try {
                if (kind == InstrumentKind::Sfz)
                    job->engine = loadSfzEngine(job->request, loadProgress_, job->error);
                else
                    job->engine = loadSf2Engine(job->request, loadProgress_, job->error);
            } catch (const std::exception& e) {
                job->error = e.what();
                job->engine.reset();
            } catch (...) {
                job->error = "an unexpected error occurred";
                job->engine.reset();
            }
        }

        {
            std::lock_guard<std::mutex> lock(loadMutex_);
            if (quit_)
                return;
            finishedJob_ = std::move(job);
        }
        host_->request_callback(host_);
    }
}

std::string Plugin::locateInstrument(const std::string& saved, const std::string& projectDir) const
{
    std::string path = joinPath(projectDir, saved); // absolute paths pass through unchanged
    if (fileExists(path))
        return path;
    // copies made by REAPER live in SoundFonts/ or SFZ/<instrument>/; let REAPER search there too
    std::string hint = kindForPath(path) == InstrumentKind::Sfz ? "SFZ" : "SoundFonts";
    auto slash = saved.find_last_of('/');
    if (slash != std::string::npos && saved[0] != '/' && saved.rfind("..", 0) != 0)
        hint = saved.substr(0, slash);
    return reaper_.resolveFile(path, hint);
}

void Plugin::resolvePendingPath()
{
    if (unresolvedPath_.empty())
        return;
    std::string dir = reaper_.projectDirectory();
    if (dir.empty())
        return;
    std::string path = locateInstrument(unresolvedPath_, dir);
    unresolvedPath_.clear();
    startLoad(path, false);
}

void Plugin::instrumentRenamed(const char* path)
{
    // REAPER copied or moved the instrument during "save as" before writing the project, so the
    // project stores the new path. Reload once the save is done: an SFZ's samples stream from disk,
    // and "move" removes the originals.
    requestedPath_ = path;
    info_.path = path;
    reloadAfterCopy_ = true;
    host_->request_callback(host_);
    bumpRevision();
}

void Plugin::registerProjectFiles()
{
    std::vector<ReaperApi::ProjectFile> files;
    if (info_.kind == InstrumentKind::Sfz) {
        // keep the instrument's folder layout so its samples and includes are found from the copy
        std::string root = commonDirectory(info_.files);
        std::string name = fileName(root);
        for (const auto& f : info_.files) {
            std::string dir = f.substr(0, f.find_last_of('/'));
            std::string sub = "SFZ/" + name + (dir.size() > root.size() ? dir.substr(root.size()) : "");
            files.push_back({ f, sub });
        }
    } else if (!info_.path.empty()) {
        files.push_back({ info_.path, "SoundFonts" });
    }
    reaper_.setProjectFiles(files);
}

std::string Plugin::projectLocationText() const
{
    if (!reaper_.available() || requestedPath_.empty() || info_.kind == InstrumentKind::None)
        return {};
    const char* how = "use Save Project As with \u201cCopy all media into project directory\u201d";
    std::string dir = reaper_.projectDirectory();
    if (dir.empty())
        return std::string("The project hasn't been saved yet. To keep this instrument with the project, ") + how + ".";
    if (requestedPath_.compare(0, dir.size() + 1, dir + "/") == 0)
        return "Stored in the project folder: " + requestedPath_.substr(dir.size() + 1);
    return std::string("Stored outside the project folder. To copy it into the project, ") + how + ".";
}

void Plugin::onTimer(clap_id id)
{
    if (id != resolveTimer_)
        return;
    resolvePendingPath();
    if (unresolvedPath_.empty() && hostTimer_) {
        hostTimer_->unregister_timer(host_, resolveTimer_);
        resolveTimer_ = CLAP_INVALID_ID;
    }
}

void Plugin::publishEngine(Engine* e)
{
    if (!active_.load()) {
        delete pending_.exchange(nullptr);
        delete current_;
        current_ = e;
        engineFresh_ = true;
        return;
    }
    delete pending_.exchange(e); // a pending engine the audio thread never took is safe to free
}

void Plugin::collectRetired()
{
    for (auto& slot : retired_)
        delete slot.exchange(nullptr);
}

void Plugin::finishLoad(std::unique_ptr<LoadJob> job)
{
    if (job->generation != loadGeneration_.load())
        return; // superseded by a newer request

    const std::string name = fileName(job->request.path);

    if (!job->engine) {
        std::string msg = "Could not load " + name + ": " + job->error + ".";
        if (info_.kind != InstrumentKind::None)
            msg += " " + info_.title + " is still loaded.";
        logMessage(CLAP_LOG_ERROR, msg);
        setStatus(LoadState::Error, msg);
        if (hostPresetLoad_ && job->preset)
            hostPresetLoad_->on_error(host_, job->preset->kind, job->preset->location.c_str(), job->preset->key.c_str(), 0, msg.c_str());
        return;
    }

    Engine* engine = job->engine.release();
    engine->prepare(sampleRate_, maxFrames_);
    loadedPolyphony_ = engine->fixedPolyphony();

    info_ = engine->info();
    if (info_.title.empty())
        info_.title = name;

    // controls start at the instrument's defaults unless restored from a project
    for (int i = 0; i < kNumControls; ++i) {
        clap_id id = kControlFirst + (clap_id)i;
        if ((size_t)i >= info_.controls.size())
            setParamValue(id, 0);
        else if (job->applyDefaults || !controlFromState_[(size_t)i])
            setParamValue(id, std::round(info_.controls[(size_t)i].defaultValue));
    }
    controlFromState_.fill(true); // from now on the current values are what we keep
    if (job->applyDefaults) {
        setParamValue(kPreset, 0);
        pendingPresetBankProgram_ = -1;
    }
    applyPendingPreset();
    applyPendingControls();
    if (!info_.presets.empty() && param(kPreset) >= (double)info_.presets.size())
        setParamValue(kPreset, (double)info_.presets.size() - 1);

    settingsDirty_.store(true);
    publishEngine(engine);

    // SFZ instruments are a folder of samples; REAPER would copy only the .sfz, so register single files only
    registerProjectFiles();

    if (hostParams_)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT | CLAP_PARAM_RESCAN_INFO);
    if (hostNoteName_)
        hostNoteName_->changed(host_);
    if (hostState_ && job->applyDefaults)
        hostState_->mark_dirty(host_);

    std::string msg = "Loaded " + info_.title + ". " + info_.summary + ".";
    for (const auto& w : info_.warnings) {
        msg += " Note: " + w + ".";
        logMessage(CLAP_LOG_WARNING, info_.title + ": " + w);
    }
    setStatus(LoadState::Ready, msg);
    addRecentInstrument(job->request.path);
    if (hostPresetLoad_ && job->preset)
        hostPresetLoad_->loaded(host_, job->preset->kind, job->preset->location.c_str(), job->preset->key.c_str());
}

void Plugin::onMainThread()
{
    collectRetired();

    std::unique_ptr<LoadJob> job;
    {
        std::lock_guard<std::mutex> lock(loadMutex_);
        job = std::move(finishedJob_);
    }
    if (job)
        finishLoad(std::move(job));

    if (reloadAfterCopy_) {
        reloadAfterCopy_ = false;
        startLoad(requestedPath_, false);
    }

    if (polyphonyReload_.exchange(false)) {
        // sfizz cannot change its voice count while running, so rebuild it
        int poly = (int)param(kPolyphony);
        if (loadedPolyphony_ > 0 && poly != loadedPolyphony_ && !requestedPath_.empty())
            startLoad(requestedPath_, false);
    }
}

bool Plugin::presetLoad(uint32_t kind, const char* location, const char* key)
{
    // PLUGIN: one of our recent instruments, keyed by path. FILE: a file the host imports.
    const char* path = kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN ? key : location;
    if ((kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN && kind != CLAP_PRESET_DISCOVERY_LOCATION_FILE) || !path || !*path)
        return false;
    unresolvedPath_.clear();
    startLoad(path, true, PresetOrigin { kind, location ? location : "", key ? key : "" });
    return true;
}

void Plugin::applyPendingPreset()
{
    if (pendingPresetBankProgram_ < 0 || info_.presets.empty())
        return;
    for (size_t i = 0; i < info_.presets.size(); ++i) {
        const PresetInfo& p = info_.presets[i];
        if (p.bank * 128 + p.program == pendingPresetBankProgram_) {
            setParamValue(kPreset, (double)i);
            settingsDirty_.store(true);
            break;
        }
    }
    pendingPresetBankProgram_ = -1;
}

void Plugin::applyPendingControls()
{
    for (size_t i = 0; i < info_.controls.size() && i < (size_t)kNumControls; ++i) {
        auto it = pendingControlValues_.find(info_.controls[i].cc);
        if (it != pendingControlValues_.end())
            setParamValue(kControlFirst + (clap_id)i, it->second);
    }
    pendingControlValues_.clear();
    settingsDirty_.store(true);
}

// --- note names

uint32_t Plugin::noteNameCount() const
{
    return (uint32_t)info_.keyNames.size();
}

bool Plugin::noteNameGet(uint32_t index, clap_note_name* n) const
{
    if (index >= info_.keyNames.size())
        return false;
    const KeyName& k = info_.keyNames[index];
    std::snprintf(n->name, sizeof(n->name), "%s", k.name.c_str());
    n->port = -1;
    n->key = (int16_t)k.key;
    n->channel = -1;
    return true;
}

// --- GUI

bool Plugin::guiCreate()
{
    if (!gui_)
        gui_ = Gui::create(*this);
    return gui_ != nullptr;
}

void Plugin::guiDestroy()
{
    delete gui_;
    gui_ = nullptr;
}

bool Plugin::guiSetParent(const clap_window* window)
{
    return gui_ && window && gui_->attach(window->cocoa);
}

bool Plugin::guiShow()
{
    if (gui_)
        gui_->setVisible(true);
    return gui_ != nullptr;
}

bool Plugin::guiHide()
{
    if (gui_)
        gui_->setVisible(false);
    return gui_ != nullptr;
}

void Plugin::guiGetSize(uint32_t* w, uint32_t* h) const
{
    Gui::preferredSize(w, h);
}

} // namespace sfp
