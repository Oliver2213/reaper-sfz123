// Checks the SFZ opcodes our sfizz patch adds: each must be recognised and audibly do its job.
// Instruments use sfizz's built-in *sine generator, so no sample files are needed.
//   sfz123_opcode_test
#include <sfizz.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <unistd.h>
#include <string>
#include <vector>

namespace {

int failures = 0;

void check(bool ok, const std::string& what)
{
    std::printf("%s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok)
        ++failures;
}

constexpr float kRate = 48000;
constexpr int kBlock = 256;

struct Render {
    std::vector<float> left, right;

    float rms(const std::vector<float>& ch, size_t from = 0, size_t to = SIZE_MAX) const
    {
        to = std::min(to, ch.size());
        double sum = 0;
        for (size_t i = from; i < to; ++i)
            sum += ch[i] * ch[i];
        return to > from ? (float)std::sqrt(sum / double(to - from)) : 0.0f;
    }
    float rmsL(size_t from = 0, size_t to = SIZE_MAX) const { return rms(left, from, to); }

    // level swing in dB between the loudest and quietest 5 ms window, after `skip` seconds
    float swingDb(float skip = 0.3f) const
    {
        const size_t win = size_t(0.005f * kRate);
        float lo = 1e9f, hi = 0;
        for (size_t i = size_t(skip * kRate); i + win <= left.size(); i += win) {
            float r = rms(left, i, i + win);
            lo = std::min(lo, r);
            hi = std::max(hi, r);
        }
        return 20 * std::log10((hi + 1e-9f) / (lo + 1e-9f));
    }

    // number of times the level crosses its median: twice the wobble rate times the duration
    int wobbles(float skip = 0.3f) const
    {
        const size_t win = size_t(0.005f * kRate);
        std::vector<float> levels;
        for (size_t i = size_t(skip * kRate); i + win <= left.size(); i += win)
            levels.push_back(rms(left, i, i + win));
        std::vector<float> sorted = levels;
        std::sort(sorted.begin(), sorted.end());
        float median = sorted.empty() ? 0 : sorted[sorted.size() / 2];
        int crossings = 0;
        for (size_t i = 1; i < levels.size(); ++i)
            crossings += (levels[i - 1] < median) != (levels[i] < median);
        return crossings;
    }
    float rmsR(size_t from = 0, size_t to = SIZE_MAX) const { return rms(right, from, to); }
};

struct Synth {
    sfz::Sfizz s;
    explicit Synth(const std::string& sfz, const std::string& path = "/virtual/test.sfz")
    {
        s.setSampleRate(kRate);
        s.setSamplesPerBlock(kBlock);
        loaded = s.loadSfzString(path, sfz);
    }
    bool loaded = false;

    bool knows(const std::string& opcode) const
    {
        for (const auto& op : s.getUnknownOpcodes())
            if (op == opcode)
                return false;
        return true;
    }

    // sample index where the sound starts
    static size_t onset(const Render& r)
    {
        for (size_t i = 0; i < r.left.size(); ++i)
            if (std::fabs(r.left[i]) > 1e-4f)
                return i;
        return r.left.size();
    }

    // plays `key` for `seconds`, with CCs set first
    Render play(int key, float seconds, std::vector<std::pair<int, int>> ccs = {}, int velocity = 100)
    {
        for (auto [cc, v] : ccs)
            s.cc(0, cc, v);
        Render r;
        std::vector<float> l(kBlock), rr(kBlock);
        float* out[2] = { l.data(), rr.data() };
        s.noteOn(0, key, velocity);
        for (int n = 0; n < int(seconds * kRate / kBlock); ++n) {
            s.renderBlock(out, kBlock);
            r.left.insert(r.left.end(), l.begin(), l.end());
            r.right.insert(r.right.end(), rr.begin(), rr.end());
        }
        s.allSoundOff();
        return r;
    }
};

void panKeytrack()
{
    Synth s("<region> sample=*sine pan_keycenter=60 pan_keytrack=10");
    check(s.loaded && s.knows("pan_keytrack") && s.knows("pan_keycenter"), "pan_keytrack, pan_keycenter recognised");
    Render center = s.play(60, 0.2f), high = s.play(70, 0.2f), low = s.play(50, 0.2f);
    check(std::fabs(center.rmsL() - center.rmsR()) < 0.01f * center.rmsL(), "pan_keytrack: key center plays in the middle");
    check(high.rmsR() > 10 * high.rmsL(), "pan_keytrack: 10 keys up is hard right");
    check(low.rmsL() > 10 * low.rmsR(), "pan_keytrack: 10 keys down is hard left");
}

void filegDepthCc()
{
    Synth s("<region> sample=*saw cutoff=200 fileg_attack=0 fileg_sustain=100 fileg_depthcc20=4800");
    check(s.loaded && s.knows("fileg_depthcc20"), "fileg_depthccN recognised (SFZ v1 spelling of fileg_depth_onccN)");
    Render closed = s.play(60, 0.2f, { { 20, 0 } }), open = s.play(60, 0.2f, { { 20, 127 } });
    check(open.rmsL() > 1.5f * closed.rmsL(), "fileg_depthccN: CC 20 opens the filter");
}

// a mono 16-bit WAV: half a second of silence, then half a second of tone
std::string writeHalfSilentWav()
{
    char dir[] = "/tmp/sfz123-opcode-test-XXXXXX";
    std::string path = std::string(mkdtemp(dir)) + "/half.wav";
    const uint32_t frames = uint32_t(kRate), bytes = frames * 2;
    FILE* f = std::fopen(path.c_str(), "wb");
    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
    std::fwrite("RIFF", 1, 4, f), u32(36 + bytes), std::fwrite("WAVEfmt ", 1, 8, f);
    u32(16), u16(1), u16(1), u32(uint32_t(kRate)), u32(uint32_t(kRate) * 2), u16(2), u16(16);
    std::fwrite("data", 1, 4, f), u32(bytes);
    for (uint32_t i = 0; i < frames; ++i)
        u16(uint16_t(int16_t(i < frames / 2 ? 0 : 8000 * std::sin(2 * M_PI * 440 * i / kRate))));
    std::fclose(f);
    return path;
}

// curve 9 is a step: 0 below CC value 64, full above
const char* kStepCurve = "<curve> curve_index=9 v000=0 v063=0 v064=1 v127=1\n";

void offsetCurveCc()
{
    std::string wav = writeHalfSilentWav();
    std::string dir = wav.substr(0, wav.rfind('/'));
    std::string region = "<region> sample=half.wav offset_oncc20=24000 ";
    Synth linear(region, dir + "/test.sfz");
    Synth curved(std::string(kStepCurve) + region + "offset_curvecc20=9", dir + "/test.sfz");
    check(curved.loaded && curved.knows("offset_curvecc20"), "offset_curveccN recognised");
    // CC 20 at 50: linear skips 9449 frames of the silence, the step curve none
    size_t a = Synth::onset(linear.play(60, 0.8f, { { 20, 50 } })), b = Synth::onset(curved.play(60, 0.8f, { { 20, 50 } }));
    check(a > 13000 && a < 16000, "offset_onccN: linear CC offset (sound after " + std::to_string(a) + " frames)");
    check(b > 23000 && b < 25000, "offset_curveccN: the curve maps CC 20 = 50 to no offset (sound after " + std::to_string(b) + " frames)");
    std::remove(wav.c_str());
    rmdir(dir.c_str());
}

void delayCurveCc()
{
    std::string region = "<region> sample=*sine delay_oncc20=0.5 ";
    Synth linear(region), curved(std::string(kStepCurve) + region + "delay_curvecc20=9");
    check(curved.knows("delay_curvecc20"), "delay_curveccN recognised");
    size_t a = Synth::onset(linear.play(60, 0.5f, { { 20, 50 } })), b = Synth::onset(curved.play(60, 0.5f, { { 20, 50 } }));
    check(a > 8000 && a < 10000, "delay_onccN: linear CC delay (" + std::to_string(a) + " frames)");
    check(b < 100, "delay_curveccN: the curve maps CC 20 = 50 to no delay (" + std::to_string(b) + " frames)");
}

// LFO 1 wobbles the volume by up to 12 dB; LFO 2 (constant 1: a sine stopped at its peak) drives it
const char* kWobble = "<region> sample=*sine lfo01_wave=1 lfo01_volume=12 lfo02_wave=1 lfo02_freq=0 lfo02_phase=0.25 ";

void lfoFreqLfo()
{
    Synth still(std::string(kWobble) + "lfo01_freq=0");
    Synth driven(std::string(kWobble) + "lfo01_freq=0 lfo02_freq_lfo01=8");
    check(driven.loaded && driven.knows("lfo02_freq_lfo01"), "lfoN_freq_lfoX recognised");
    Render a = still.play(60, 1.3f), b = driven.play(60, 1.3f);
    check(a.swingDb() < 1, "lfoN_freq_lfoX: a stopped LFO doesn't wobble");
    check(b.swingDb() > 10 && b.wobbles() >= 12 && b.wobbles() <= 20, "lfoN_freq_lfoX: LFO 2 runs LFO 1 at 8 Hz");

    Synth cc(std::string(kWobble) + "lfo01_freq=0 lfo02_freq_lfo01_oncc20=8");
    check(cc.knows("lfo02_freq_lfo01_oncc20"), "lfoN_freq_lfoX_onccY recognised");
    check(cc.play(60, 1.3f, { { 20, 0 } }).swingDb() < 1 && cc.play(60, 1.3f, { { 20, 127 } }).swingDb() > 10,
        "lfoN_freq_lfoX_onccY: CC 20 sets how much LFO 2 speeds up LFO 1");
}

void lfoDepthLfo()
{
    Synth full(std::string(kWobble) + "lfo01_freq=8 lfo02_depth_lfo01=100");
    Synth none(std::string(kWobble) + "lfo01_freq=8 lfo02_depth_lfo01=0");
    check(full.knows("lfo02_depth_lfo01"), "lfoN_depth_lfoX recognised");
    check(full.play(60, 1.3f).swingDb() > 10, "lfoN_depth_lfoX=100 keeps LFO 1 at full depth");
    check(none.play(60, 1.3f).swingDb() < 1, "lfoN_depth_lfoX=0 silences LFO 1");
}

void egDepthLfo()
{
    const std::string eg = "eg01_time1=0 eg01_sustain=1 eg01_level0_oncc20=1 eg01_level1_oncc20=1 eg01_depth_lfo01=100";
    Synth s(std::string(kWobble) + "lfo01_freq=8 " + eg);
    check(s.knows("eg01_depth_lfo01"), "egN_depth_lfoX recognised");
    check(s.play(60, 1.3f, { { 20, 0 } }).swingDb() < 1 && s.play(60, 1.3f, { { 20, 127 } }).swingDb() > 10,
        "egN_depth_lfoX: an EG held at CC 20's level scales LFO 1");
}

} // namespace

int main()
{
    panKeytrack();
    filegDepthCc();
    lfoFreqLfo();
    lfoDepthLfo();
    egDepthLfo();
    offsetCurveCc();
    delayCurveCc();
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
