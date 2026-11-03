// Plug-in state as readable "key=value" lines: the instrument path (relative to the REAPER project
// folder when possible, so projects stay portable), the preset as bank:program, control values by
// CC number, and every parameter as p<id>.
#include "plugin.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace sfp {

bool Plugin::stateSave(const clap_ostream* stream, bool projectRelative)
{
    // presets are loaded into other projects, so only project state is stored project-relative
    std::string path = unresolvedPath_;
    if (path.empty()) {
        std::string rel = projectRelative ? relativePath(requestedPath_, reaper_.projectDirectory()) : std::string();
        path = rel.empty() ? requestedPath_ : rel; // absolute only until the project is first saved
    }

    std::ostringstream os;
    os.precision(17);
    os << "SFPLAYER 1\n";
    os << "path=" << path << "\n";
    if (!info_.presets.empty()) {
        size_t i = (size_t)std::clamp((long)param(kPreset), 0L, (long)info_.presets.size() - 1);
        os << "preset=" << info_.presets[i].bank << ":" << info_.presets[i].program << "\n";
    }
    for (size_t i = 0; i < info_.controls.size() && i < (size_t)kNumControls; ++i)
        os << "cc" << info_.controls[i].cc << "=" << param(kControlFirst + (clap_id)i) << "\n";
    for (const auto& d : paramDefs())
        os << "p" << d.id << "=" << param(d.id) << "\n";

    const std::string s = os.str();
    const char* data = s.data();
    size_t left = s.size();
    while (left > 0) {
        int64_t n = stream->write(stream, data, left);
        if (n <= 0)
            return false;
        data += n;
        left -= (size_t)n;
    }
    return true;
}

bool Plugin::stateLoad(const clap_istream* stream)
{
    std::string s;
    char buf[4096];
    for (;;) {
        int64_t n = stream->read(stream, buf, sizeof(buf));
        if (n < 0)
            return false;
        if (n == 0)
            break;
        s.append(buf, (size_t)n);
    }

    std::istringstream is(s);
    std::string line;
    if (!std::getline(is, line) || line.rfind("SFPLAYER", 0) != 0)
        return false;

    std::string path;
    bool havePath = false;

    // start from defaults so values missing from older states are well defined
    for (const auto& d : paramDefs())
        setParamValue(d.id, d.def);
    controlFromState_.fill(false);
    pendingPresetBankProgram_ = -1;
    pendingControlValues_.clear();

    while (std::getline(is, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (key == "path") {
            path = value;
            havePath = true;
        } else if (key == "preset") {
            int bank = 0, program = 0;
            if (std::sscanf(value.c_str(), "%d:%d", &bank, &program) == 2)
                pendingPresetBankProgram_ = bank * 128 + program;
        } else if (key.size() > 2 && key.compare(0, 2, "cc") == 0) {
            pendingControlValues_[std::atoi(key.c_str() + 2)] = std::strtod(value.c_str(), nullptr);
        } else if (key.size() > 1 && key[0] == 'p') {
            char* end = nullptr;
            long id = std::strtol(key.c_str() + 1, &end, 10);
            if (end && *end == 0) {
                setParamValue((clap_id)id, std::strtod(value.c_str(), nullptr));
                if (id >= kControlFirst && id < kControlFirst + kNumControls)
                    controlFromState_[(size_t)(id - kControlFirst)] = true;
            }
        }
    }

    settingsDirty_.store(true);
    bumpRevision();

    unresolvedPath_.clear();
    std::string projectDir = reaper_.projectDirectory();
    // REAPER may not know the project folder while the project is still opening
    bool waitForProject = !path.empty() && path[0] != '/' && projectDir.empty();
    if (!waitForProject && !path.empty())
        path = locateInstrument(path, projectDir);

    if (!havePath || path.empty()) {
        if (!requestedPath_.empty())
            unloadFile();
    } else if (waitForProject) {
        unresolvedPath_ = path;
        setStatus(LoadState::Loading, "Waiting for the project folder to find " + path + ".");
        if (hostTimer_ && resolveTimer_ == CLAP_INVALID_ID)
            hostTimer_->register_timer(host_, 500, &resolveTimer_);
    } else if (path != requestedPath_ || loadState_ == LoadState::Error) {
        startLoad(path, false);
    } else if (loadedPolyphony_ > 0 && loadedPolyphony_ != (int)param(kPolyphony)) {
        startLoad(path, false);
    } else {
        applyPendingPreset(); // same file already loaded
        applyPendingControls();
    }

    if (hostParams_)
        hostParams_->rescan(host_, CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_TEXT);
    return true;
}

} // namespace sfp
