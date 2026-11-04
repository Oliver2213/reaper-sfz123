// Minimal headless CLAP host used to test the built plugin bundle.
//
//   sfz123_test "<path to sfz123.clap>" <instrument> [out.wav]
//
// Loads the instrument through the plugin's state (as a project reload would), plays a few
// notes through the CLAP API and checks the output, optionally writing it to a WAV file.
#include <clap/clap.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <filesystem>
#include <map>
#include <string>
#include <thread>
#include <vector>

static std::atomic<bool> gCallback { false };
static std::atomic<int> gRescans { 0 };

static const void* hostGetExtension(const clap_host*, const char* id);
static void hostRequestRestart(const clap_host*) {}
static void hostRequestProcess(const clap_host*) {}
static void hostRequestCallback(const clap_host*) { gCallback = true; }

static clap_host gHost = { CLAP_VERSION_INIT, nullptr, "sfz123_test", "test", "", "1", hostGetExtension, hostRequestRestart,
    hostRequestProcess, hostRequestCallback };

static void logFn(const clap_host*, clap_log_severity s, const char* msg) { std::printf("  [plugin log %d] %s\n", (int)s, msg); }
static const clap_host_log gLog = { logFn };
static void rescan(const clap_host*, clap_param_rescan_flags) { gRescans++; }
static void clearParams(const clap_host*, clap_id, clap_param_clear_flags) {}
static void requestFlush(const clap_host*) {}
static const clap_host_params gParams = { rescan, clearParams, requestFlush };

// Stand-in for REAPER's "cockos.reaper_extension", reporting the project file in SFP_FAKE_PROJECT.
static int gFakeProject;
static void* fakeContext(const clap_host*, int sel) { return sel == 3 ? &gFakeProject : nullptr; }
static void* fakeEnumProjects(int idx, char* fn, int size)
{
    if (idx != 0)
        return nullptr;
    if (fn)
        std::snprintf(fn, (size_t)size, "%s", getenv("SFP_FAKE_PROJECT"));
    return &gFakeProject;
}
// resolve_fn2 stand-in: finds a missing file by name in SFP_FAKE_MEDIA_DIR
static void fakeResolve(const char* in, char* out, int size, const char*)
{
    std::string name = std::string(in).substr(std::string(in).find_last_of('/') + 1);
    const char* dir = getenv("SFP_FAKE_MEDIA_DIR");
    std::string found = dir ? std::string(dir) + "/" + name : in;
    FILE* f = std::fopen(found.c_str(), "rb");
    std::snprintf(out, (size_t)size, "%s", f ? found.c_str() : in);
    if (f)
        std::fclose(f);
}
// file_in_project_ex2 registrations: userdata -> {path, callback}
using FileCallback = intptr_t (*)(void*, int, void*);
static std::map<void*, std::pair<std::string, FileCallback>> gRegistered;
static int fakeRegister(const char* name, void* info)
{
    void** p = (void**)info;
    if (!std::strcmp(name, "file_in_project_ex2"))
        gRegistered[p[2]] = { (const char*)p[0], (FileCallback)p[3] };
    else if (!std::strcmp(name, "-file_in_project_ex2"))
        gRegistered.erase(p[2]);
    return 1;
}

// What REAPER's "Save as, copy all media into project directory" does for plug-in files.
static void fakeSaveAsWithCopy(const std::string& newProjectDir)
{
    namespace fs = std::filesystem;
    std::string main;
    for (auto& [ud, reg] : gRegistered) {
        const char* sub = (const char*)reg.second(ud, 0x100, nullptr);
        fs::path dest = fs::path(newProjectDir) / (sub ? sub : "") / fs::path(reg.first).filename();
        fs::create_directories(dest.parent_path());
        fs::copy_file(reg.first, dest, fs::copy_options::overwrite_existing);
        if (main.empty())
            main = dest.string();
        reg.second(ud, 0, (void*)dest.c_str());
    }
    std::printf("  [reaper] copied %zu files into %s\n", gRegistered.size(), newProjectDir.c_str());
    setenv("SFP_FAKE_PROJECT", (newProjectDir + "/song.rpp").c_str(), 1);
}
static void* fakeGetFunc(const char* name)
{
    if (!std::strcmp(name, "resolve_fn2"))
        return (void*)fakeResolve;
    if (!std::strcmp(name, "clap_get_reaper_context"))
        return (void*)fakeContext;
    if (!std::strcmp(name, "EnumProjects"))
        return (void*)fakeEnumProjects;
    return nullptr;
}
static struct {
    int version;
    void* hwnd;
    int (*reg)(const char*, void*);
    void* (*getFunc)(const char*);
} gFakeReaper = { 0x20E, nullptr, fakeRegister, fakeGetFunc };

static const void* hostGetExtension(const clap_host*, const char* id)
{
    if (!std::strcmp(id, "cockos.reaper_extension") && getenv("SFP_FAKE_PROJECT"))
        return &gFakeReaper;
    if (!std::strcmp(id, CLAP_EXT_LOG))
        return &gLog;
    if (!std::strcmp(id, CLAP_EXT_PARAMS))
        return &gParams;
    return nullptr;
}

// ---- event list
struct Events {
    std::vector<std::vector<uint8_t>> storage;
    void midi(uint32_t time, uint8_t a, uint8_t b, uint8_t c)
    {
        clap_event_midi e {};
        e.header = { sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0 };
        e.port_index = 0;
        e.data[0] = a;
        e.data[1] = b;
        e.data[2] = c;
        push(&e.header);
    }
    void param(uint32_t time, clap_id id, double v)
    {
        clap_event_param_value e {};
        e.header = { sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0 };
        e.param_id = id;
        e.note_id = -1;
        e.port_index = e.channel = e.key = -1;
        e.value = v;
        push(&e.header);
    }
    void push(const clap_event_header* h)
    {
        storage.emplace_back((const uint8_t*)h, (const uint8_t*)h + h->size);
    }
    static uint32_t size(const clap_input_events* l) { return (uint32_t)((Events*)l->ctx)->storage.size(); }
    static const clap_event_header* get(const clap_input_events* l, uint32_t i)
    {
        return (const clap_event_header*)((Events*)l->ctx)->storage[i].data();
    }
};

static std::vector<std::string> gOutEvents;
static bool outPush(const clap_output_events*, const clap_event_header* h)
{
    if (h->type == CLAP_EVENT_PARAM_VALUE) {
        auto* e = (const clap_event_param_value*)h;
        gOutEvents.push_back("param " + std::to_string(e->param_id) + " = " + std::to_string(e->value));
    }
    return true;
}

struct Stream {
    std::string data;
    size_t pos = 0;
};
static int64_t sWrite(const clap_ostream* s, const void* buf, uint64_t n)
{
    ((Stream*)s->ctx)->data.append((const char*)buf, n);
    return (int64_t)n;
}
static int64_t sRead(const clap_istream* s, void* buf, uint64_t n)
{
    Stream* st = (Stream*)s->ctx;
    uint64_t left = st->data.size() - st->pos;
    n = std::min(n, left);
    std::memcpy(buf, st->data.data() + st->pos, n);
    st->pos += n;
    return (int64_t)n;
}

static void writeWav(const char* path, const std::vector<float>& l, const std::vector<float>& r, int sr)
{
    FILE* f = std::fopen(path, "wb");
    if (!f)
        return;
    uint32_t n = (uint32_t)l.size();
    uint32_t dataBytes = n * 2 * 2;
    auto w32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto w16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f);
    w32(36 + dataBytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    w32(16);
    w16(1);
    w16(2);
    w32((uint32_t)sr);
    w32((uint32_t)sr * 4);
    w16(4);
    w16(16);
    std::fwrite("data", 1, 4, f);
    w32(dataBytes);
    for (uint32_t i = 0; i < n; ++i) {
        int16_t a = (int16_t)std::lround(std::clamp(l[i], -1.0f, 1.0f) * 32767);
        int16_t b = (int16_t)std::lround(std::clamp(r[i], -1.0f, 1.0f) * 32767);
        w16((uint16_t)a);
        w16((uint16_t)b);
    }
    std::fclose(f);
}

int main(int argc, char** argv)
{
    setenv("SFP_NO_RECENTS", "1", 1); // keep test loads out of the user's recent instruments
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <plugin.clap> <instrument> [out.wav]\n", argv[0]);
        return 2;
    }
    std::string bundle = argv[1];
    std::string exe = bundle + "/Contents/MacOS/sfz123";
    void* lib = dlopen(exe.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }
    auto* entry = (const clap_plugin_entry*)dlsym(lib, "clap_entry");
    if (!entry || !entry->init(bundle.c_str()))
        return 1;
    auto* factory = (const clap_plugin_factory*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor* desc = factory->get_plugin_descriptor(factory, 0);
    std::printf("plugin: %s (%s) %s\n", desc->name, desc->id, desc->version);

    const clap_plugin* p = factory->create_plugin(factory, &gHost, desc->id);
    if (!p || !p->init(p))
        return 1;

    auto* params = (const clap_plugin_params*)p->get_extension(p, CLAP_EXT_PARAMS);
    auto* state = (const clap_plugin_state*)p->get_extension(p, CLAP_EXT_STATE);
    auto* noteNames = (const clap_plugin_note_name*)p->get_extension(p, CLAP_EXT_NOTE_NAME);

    std::printf("%u parameters\n", params->count(p));

    const int sr = 48000;
    const uint32_t block = 512;
    p->activate(p, sr, 1, block);
    p->start_processing(p);

    // load the instrument via state, like REAPER restoring a project
    Stream st;
    st.data = std::string("SFZ123 1\npath=") + argv[2] + "\n";
    if (const char* extra = getenv("SFP_STATE_EXTRA")) // e.g. "preset=0:3\np10=0\n"
        st.data += extra;
    clap_istream is { &st, sRead };
    auto t0 = std::chrono::steady_clock::now();
    if (!state->load(p, &is)) {
        std::fprintf(stderr, "state load failed\n");
        return 1;
    }

    std::vector<float> L, R;
    std::vector<float> bl(block), br(block);
    float* chans[2] = { bl.data(), br.data() };
    clap_audio_buffer out {};
    out.data32 = chans;
    out.channel_count = 2;
    clap_output_events oev { nullptr, outPush };

    auto run = [&](Events& ev, uint32_t blocks, bool keep) {
        for (uint32_t b = 0; b < blocks; ++b) {
            if (gCallback.exchange(false))
                p->on_main_thread(p);
            clap_input_events iev { &ev, Events::size, Events::get };
            Events empty;
            clap_input_events none { &empty, Events::size, Events::get };
            clap_process pr {};
            pr.steady_time = -1;
            pr.frames_count = block;
            pr.audio_outputs = &out;
            pr.audio_outputs_count = 1;
            pr.in_events = b == 0 ? &iev : &none;
            pr.out_events = &oev;
            p->process(p, &pr);
            if (keep) {
                L.insert(L.end(), bl.begin(), bl.end());
                R.insert(R.end(), br.begin(), br.end());
            }
        }
    };

    // wait (while processing silence) until the load finishes
    char text[256];
    bool loaded = false;
    for (int i = 0; i < 3000 && !loaded; ++i) {
        Events none;
        run(none, 1, false);
        // loaded once control slot 1 has a real name or presets exist
        clap_param_info info {};
        for (uint32_t k = 0; k < params->count(p); ++k) {
            params->get_info(p, k, &info);
            if (info.id == 100)
                break;
        }
        params->value_to_text(p, 10, 0, text, sizeof(text));
        loaded = std::strstr(info.name, "unused") == nullptr || std::strcmp(text, "No preset") != 0;
        if (!loaded)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    if (!loaded) {
        std::fprintf(stderr, "FAIL: instrument did not load\n");
        return 1;
    }
    std::printf("loaded in %.2f s\n", secs);

    // describe what the host sees
    for (uint32_t i = 0; i < params->count(p); ++i) {
        clap_param_info info;
        params->get_info(p, i, &info);
        if (info.flags & CLAP_PARAM_IS_HIDDEN)
            continue;
        double v = 0;
        params->get_value(p, info.id, &v);
        params->value_to_text(p, info.id, v, text, sizeof(text));
        std::printf("  [%3u] %-34s %-28s %s\n", info.id, info.name, info.module, text);
    }
    // list presets (the text clamps to the last one, so stop at the first repeat)
    {
        std::string prev;
        for (int i = 0; i < 1024; ++i) {
            params->value_to_text(p, 10, i, text, sizeof(text));
            if (prev == text || !std::strcmp(text, "No preset"))
                break;
            if (i < 12)
                std::printf("  preset %d: %s\n", i, text);
            else if (i == 12)
                std::printf("  ...\n");
            prev = text;
        }
    }
    if (noteNames) {
        uint32_t n = noteNames->count(p);
        std::printf("%u note names\n", n);
        for (uint32_t i = 0; i < n && i < 8; ++i) {
            clap_note_name nn;
            noteNames->get(p, i, &nn);
            std::printf("  key %d: %s\n", nn.key, nn.name);
        }
    }

    if (const char* copyTo = getenv("SFP_FAKE_COPY_TO")) {
        std::printf("  %zu files registered with REAPER\n", gRegistered.size());
        fakeSaveAsWithCopy(copyTo);
        Stream mid;
        clap_ostream mos { &mid, sWrite };
        state->save(p, &mos);
        std::printf("  state written by save-as: %s\n", mid.data.substr(11, mid.data.find('\n', 11) - 11).c_str());
        // let the plug-in reload from the copies, as it does after REAPER's save finishes
        for (int i = 0; i < 400; ++i) {
            Events none;
            run(none, 1, false);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        size_t inCopy = 0;
        for (auto& [ud, reg] : gRegistered)
            inCopy += reg.first.rfind(copyTo, 0) == 0;
        std::printf("  after reload: %zu of %zu registered files are the copies\n", inCopy, gRegistered.size());
    }

    // play a chord with the sustain pedal, then release
    Events chord;
    chord.midi(0, 0x90, 60, 100);
    chord.midi(10, 0x90, 64, 90);
    chord.midi(20, 0x90, 67, 80);
    run(chord, 20, true);
    Events off;
    off.midi(0, 0xB0, 64, 127); // pedal down
    off.midi(5, 0x80, 60, 0);
    off.midi(5, 0x80, 64, 0);
    off.midi(5, 0x80, 67, 0);
    run(off, 80, true);
    Events pedalUp;
    pedalUp.midi(0, 0xB0, 64, 0);
    run(pedalUp, 300, true);

    // a melody at a quieter volume, via a parameter event
    Events vol;
    vol.param(0, 0, -6.0);
    vol.midi(0, 0x90, 72, 110);
    run(vol, 30, true);
    Events v2;
    v2.midi(0, 0x80, 72, 0);
    v2.midi(0, 0x90, 76, 70);
    run(v2, 30, true);
    Events v3;
    v3.midi(0, 0x80, 76, 0);
    v3.midi(0, 0xC0, 40, 0); // program change (SF2: violin in GM)
    v3.midi(10, 0x90, 79, 100);
    run(v3, 60, true);
    Events v4;
    v4.midi(0, 0x80, 79, 0);
    run(v4, 200, true);

    float peak = 0;
    double sum = 0;
    bool finite = true;
    for (size_t i = 0; i < L.size(); ++i) {
        peak = std::max(peak, std::max(std::fabs(L[i]), std::fabs(R[i])));
        sum += L[i] * L[i] + R[i] * R[i];
        finite = finite && std::isfinite(L[i]) && std::isfinite(R[i]);
    }
    double rms = std::sqrt(sum / (2.0 * L.size()));
    std::printf("rendered %.1f s: peak %.3f, rms %.4f, finite %s\n", L.size() / (double)sr, peak, rms, finite ? "yes" : "NO");
    for (auto& e : gOutEvents)
        std::printf("  plugin -> host: %s\n", e.c_str());

    if (argc > 3) {
        writeWav(argv[3], L, R, sr);
        std::printf("wrote %s\n", argv[3]);
    }

    // state round trip
    Stream saved;
    clap_ostream os { &saved, sWrite };
    state->save(p, &os);
    std::printf("saved state (%zu bytes):\n", saved.data.size());
    size_t start = 0;
    while (start < saved.data.size()) {
        size_t end = saved.data.find('\n', start);
        std::string line = saved.data.substr(start, end - start);
        if (line[0] != 'p' || line.rfind("path", 0) == 0 || line.rfind("preset", 0) == 0)
            std::printf("  %s\n", line.c_str());
        start = end + 1;
    }

    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    entry->deinit();

    bool ok = finite && peak > 0.001 && peak < 4.0;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
