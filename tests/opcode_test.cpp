// Checks the SFZ opcodes our sfizz patch adds: each must be recognised and audibly do its job.
// Instruments use sfizz's built-in *sine generator, so no sample files are needed.
//   sfz123_opcode_test
#include <sfizz.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
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
    float rmsR(size_t from = 0, size_t to = SIZE_MAX) const { return rms(right, from, to); }
};

constexpr float kRate = 48000;
constexpr int kBlock = 256;

struct Synth {
    sfz::Sfizz s;
    explicit Synth(const std::string& sfz)
    {
        s.setSampleRate(kRate);
        s.setSamplesPerBlock(kBlock);
        loaded = s.loadSfzString("/virtual/test.sfz", sfz);
    }
    bool loaded = false;

    bool knows(const std::string& opcode) const
    {
        for (const auto& op : s.getUnknownOpcodes())
            if (op == opcode)
                return false;
        return true;
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

} // namespace

int main()
{
    panKeytrack();
    filegDepthCc();
    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
