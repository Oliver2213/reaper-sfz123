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

    // plays `key`, releases it after `holdSeconds`, renders `seconds` in all; `second` triggers another
    // note at `secondAt` seconds if set
    Render playReleased(int key, float holdSeconds, float seconds, int second = -1, float secondAt = 0)
    {
        Render r;
        std::vector<float> l(kBlock), rr(kBlock);
        float* out[2] = { l.data(), rr.data() };
        s.noteOn(0, key, 127);
        const int blocks = int(seconds * kRate / kBlock);
        for (int n = 0; n < blocks; ++n) {
            if (n == int(holdSeconds * kRate / kBlock))
                s.noteOff(0, key, 0);
            if (second >= 0 && n == int(secondAt * kRate / kBlock))
                s.noteOn(0, second, 127);
            s.renderBlock(out, kBlock);
            r.left.insert(r.left.end(), l.begin(), l.end());
        }
        s.allSoundOff();
        return r;
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

float db(float a, float b) { return 20 * std::log10((a + 1e-9f) / (b + 1e-9f)); }

void variables()
{
    const std::string sum = "<region> sample=*sine var01_volume=-24 var01_oncc20=0.5 var01_oncc21=0.5";
    Synth add(sum);
    check(add.knows("var01_volume") && add.knows("var01_oncc20"), "varNN_target, varNN_onccX recognised");
    float quiet = add.play(60, 0.2f, { { 20, 0 }, { 21, 0 } }).rmsL();
    check(std::fabs(db(add.play(60, 0.2f, { { 20, 127 }, { 21, 0 } }).rmsL(), quiet) + 12) < 0.5f,
        "varNN (add): one CC at full gives half the amount (-12 dB)");
    check(std::fabs(db(add.play(60, 0.2f, { { 20, 127 }, { 21, 127 } }).rmsL(), quiet) + 24) < 0.5f,
        "varNN (add): both CCs at full give the whole amount (-24 dB)");

    Synth mult("<region> sample=*sine var01_mod=mult var01_volume=-24 var01_oncc20=1 var01_oncc21=1");
    check(mult.knows("var01_mod"), "varNN_mod recognised");
    float base = mult.play(60, 0.2f, { { 20, 0 }, { 21, 0 } }).rmsL();
    check(std::fabs(db(mult.play(60, 0.2f, { { 20, 127 }, { 21, 0 } }).rmsL(), base)) < 0.5f,
        "varNN (mult): nothing while the other CC is at 0");
    check(std::fabs(db(mult.play(60, 0.2f, { { 20, 127 }, { 21, 64 } }).rmsL(), base) + 12) < 1.0f,
        "varNN (mult): CC 20 full times CC 21 half is half the amount");
    check(std::fabs(db(mult.play(60, 0.2f, { { 20, 127 }, { 21, 127 } }).rmsL(), base) + 24) < 0.5f,
        "varNN (mult): both full give the whole amount");

    Synth vel("<region> sample=*sine var01_mod=mult var01_volume=-24 var01_oncc131=1 var01_oncc20=1");
    auto change = [&](int velocity) {
        return db(vel.play(60, 0.2f, { { 20, 127 } }, velocity).rmsL(), vel.play(60, 0.2f, { { 20, 0 } }, velocity).rmsL());
    };
    check(std::fabs(change(127) + 24) < 0.5f, "varNN (mult) with CC 131: a full-velocity note gets the whole amount");
    check(std::fabs(change(1)) < 0.5f, "varNN (mult) with CC 131: a velocity-1 note is barely changed");

    Synth curved(std::string(kStepCurve) + "<region> sample=*sine var01_volume=-24 var01_oncc20=1 var01_curvecc20=9");
    check(curved.knows("var01_curvecc20"), "varNN_curveccX recognised");
    float c0 = curved.play(60, 0.2f, { { 20, 0 } }).rmsL();
    check(std::fabs(db(curved.play(60, 0.2f, { { 20, 50 } }).rmsL(), c0)) < 0.5f && db(curved.play(60, 0.2f, { { 20, 100 } }).rmsL(), c0) < -23,
        "varNN_curveccX: the step curve gates CC 20");

    Synth cutoff("<region> sample=*saw cutoff=200 var01_cutoff=4800 var01_oncc20=1");
    check(cutoff.play(60, 0.2f, { { 20, 127 } }).rmsL() > 1.5f * cutoff.play(60, 0.2f, { { 20, 0 } }).rmsL(),
        "varNN_cutoff: CC 20 opens the filter");
}

// envelope level (0..1) at `t` seconds, relative to the full level at `ref` seconds
float levelAt(const Render& r, float t, float ref)
{
    const size_t win = size_t(0.004f * kRate);
    auto at = [&](float sec) { size_t i = size_t(sec * kRate); return r.rmsL(i - win / 2, i + win / 2); };
    return at(t) / at(ref);
}

void envelopeShapes()
{
    auto attackAt = [](const char* shape) {
        Synth s(std::string("<region> sample=*sine ampeg_attack=0.4 ") + shape);
        return levelAt(s.play(60, 0.6f), 0.1f, 0.5f);
    };
    check(Synth("<region> sample=*sine ampeg_attack_shape=3").knows("ampeg_attack_shape"), "ampeg_attack_shape recognised");
    float lin = attackAt(""), zero = attackAt("ampeg_attack_shape=0"), slow = attackAt("ampeg_attack_shape=5"), fast = attackAt("ampeg_attack_shape=-5");
    check(std::fabs(lin - 0.25f) < 0.03f && std::fabs(zero - 0.25f) < 0.03f, "ampeg_attack_shape=0 is linear, like no shape (" + std::to_string(zero) + ")");
    check(slow < 0.04f, "ampeg_attack_shape=5 starts slowly (" + std::to_string(slow) + " at a quarter)");
    check(std::fabs(fast - 0.72f) < 0.04f, "ampeg_attack_shape=-5 starts quickly (" + std::to_string(fast) + " at a quarter)");

    Synth decay("<region> sample=*sine ampeg_decay=0.4 ampeg_sustain=0 ampeg_decay_shape=0");
    check(decay.knows("ampeg_decay_shape"), "ampeg_decay_shape recognised");
    float d = levelAt(decay.play(60, 0.5f), 0.1f, 0.003f);
    check(std::fabs(d - 0.75f) < 0.04f, "ampeg_decay_shape=0 falls in a straight line (" + std::to_string(d) + " at a quarter)");

    Synth release("<region> sample=*sine ampeg_release=0.4 ampeg_release_shape=0");
    check(release.knows("ampeg_release_shape"), "ampeg_release_shape recognised");
    float rl = levelAt(release.playReleased(60, 0.2f, 0.7f), 0.4f, 0.15f);
    check(std::fabs(rl - 0.5f) < 0.05f, "ampeg_release_shape=0 releases in a straight line (" + std::to_string(rl) + " halfway)");
    Synth released("<region> sample=*sine ampeg_release=0.4");
    float def = levelAt(released.playReleased(60, 0.2f, 0.7f), 0.4f, 0.15f);
    check(def < 0.1f, "no ampeg_release_shape keeps sfizz's exponential release (" + std::to_string(def) + " halfway)");

    // a second note in the group mutes the first with a 0.4 s linear fade; key 72 is silent here
    Synth off("<region> key=60 sample=*sine group=1 off_by=1 off_mode=time off_time=0.4 off_shape=0 "
              "<region> key=72 sample=*sine group=1 amplitude=0");
    check(off.knows("off_shape"), "off_shape recognised");
    float o = levelAt(off.playReleased(60, 2.0f, 0.8f, 72, 0.2f), 0.4f, 0.15f);
    check(std::fabs(o - 0.5f) < 0.06f, "off_shape=0 mutes in a straight line (" + std::to_string(o) + " halfway)");
    Synth offExp("<region> key=60 sample=*sine group=1 off_by=1 off_mode=time off_time=0.4 "
                 "<region> key=72 sample=*sine group=1 amplitude=0");
    float e = levelAt(offExp.playReleased(60, 2.0f, 0.8f, 72, 0.2f), 0.4f, 0.15f);
    check(e < 0.1f, "without off_shape the mute stays exponential (" + std::to_string(e) + " halfway)");
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
    variables();
    envelopeShapes();
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
