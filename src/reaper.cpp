#include "reaper.h"

#include <cstdint>
#include <cstring>
#include <sys/stat.h>
#include <vector>

namespace sfp {

namespace {

// The leading members of REAPER's reaper_plugin_info_t (reaper_plugin.h).
struct ReaperPluginInfo {
    int caller_version;
    void* hwnd_main;
    int (*Register)(const char* name, void* infostruct);
    void* (*GetFunc)(const char* name);
};

using EnumProjectsFn = void* (*)(int idx, char* fn, int fnSize);
using GetContextFn = void* (*)(const clap_host* host, int sel);
using ResolveFn = void (*)(const char* in, char* out, int outSize, const char* checkSubDir);
using GetConfigFn = bool (*)(void* trackOrTake, int fx, const char* name, char* out, int outSize);
using SetConfigFn = bool (*)(void* trackOrTake, int fx, const char* name, const char* value);
using TrackStringFn = bool (*)(void* track, const char* name, char* value, bool set);

constexpr int kContextTrack = 1;
constexpr int kContextTake = 2;
constexpr int kContextProject = 3;
constexpr int kContextFxDsp = 4;
constexpr int kContextFxIndex = 6;
constexpr int kRenderingProject = 0x40000000;

std::string parentDir(const std::string& path)
{
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool isOurs(const char* fxName)
{
    return std::strstr(fxName, "sfz123") || std::strstr(fxName, "SF Player"); // SF Player was its old name
}

std::vector<std::string> split(const std::string& path)
{
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        if (end > start)
            parts.push_back(path.substr(start, end - start));
        start = end + 1;
    }
    return parts;
}

} // namespace

void ReaperApi::init(const clap_host* host, RenameCallback onRename, void* context)
{
    host_ = host;
    onRename_ = onRename;
    context_ = context;
    auto* info = static_cast<const ReaperPluginInfo*>(host->get_extension(host, "cockos.reaper_extension"));
    if (info && info->GetFunc) {
        getFunc_ = info->GetFunc;
        register_ = info->Register;
    }
}

void* ReaperApi::context(int sel) const
{
    auto getContext = getFunc_ ? reinterpret_cast<GetContextFn>(getFunc_("clap_get_reaper_context")) : nullptr;
    return getContext ? getContext(host_, sel) : nullptr;
}

void* ReaperApi::project() const
{
    return context(kContextProject);
}

template <typename Fn> bool ReaperApi::withFx(const char* api, Fn fn) const
{
    void* owner = context(kContextTrack);
    std::string prefix = "TrackFX_";
    if (!owner) {
        owner = context(kContextTake);
        prefix = "TakeFX_";
    }
    if (!owner)
        return false;
    int fx = (int)(intptr_t)context(kContextFxIndex) - 1; // 1-based
    auto get = reinterpret_cast<GetConfigFn>(getFunc_((prefix + "GetNamedConfigParm").c_str()));
    void* f = getFunc_((prefix + api).c_str());
    if (!get || !f)
        return false;
    // the index may not address us (FX containers have their own chains), so check it is one of ours
    char name[512] = { 0 };
    if (!get(owner, fx, "fx_name", name, sizeof(name)) || !isOurs(name))
        return false;
    return fn(f, owner, fx);
}

void* ReaperApi::firstTrackOfOurs() const
{
    void* track = context(kContextTrack);
    auto get = getFunc_ ? reinterpret_cast<GetConfigFn>(getFunc_("TrackFX_GetNamedConfigParm")) : nullptr;
    if (!track || !get)
        return nullptr;
    int fx = (int)(intptr_t)context(kContextFxIndex) - 1;
    for (int i = 0; i < fx; ++i) {
        char name[512] = { 0 };
        if (get(track, i, "fx_name", name, sizeof(name)) && isOurs(name))
            return nullptr;
    }
    return track;
}

bool ReaperApi::trackName(std::string& name) const
{
    void* track = firstTrackOfOurs();
    auto fn = track ? reinterpret_cast<TrackStringFn>(getFunc_("GetSetMediaTrackInfo_String")) : nullptr;
    char buf[1024] = { 0 };
    if (!fn || !fn(track, "P_NAME", buf, false))
        return false;
    name = buf;
    return true;
}

bool ReaperApi::setTrackName(const std::string& name)
{
    void* track = firstTrackOfOurs();
    auto fn = track ? reinterpret_cast<TrackStringFn>(getFunc_("GetSetMediaTrackInfo_String")) : nullptr;
    std::string value = name;
    return fn && fn(track, "P_NAME", value.data(), true);
}

bool ReaperApi::fxRename(std::string& name) const
{
    return withFx("GetNamedConfigParm", [&](void* f, void* owner, int fx) {
        char buf[512] = { 0 };
        if (!reinterpret_cast<GetConfigFn>(f)(owner, fx, "renamed_name", buf, sizeof(buf)))
            return false;
        name = buf;
        return true;
    });
}

bool ReaperApi::setFxRename(const std::string& name)
{
    return withFx("SetNamedConfigParm", [&](void* f, void* owner, int fx) {
        return reinterpret_cast<SetConfigFn>(f)(owner, fx, "renamed_name", name.c_str());
    });
}

std::string ReaperApi::projectDirectory() const
{
    void* proj = project();
    auto enumProjects = getFunc_ ? reinterpret_cast<EnumProjectsFn>(getFunc_("EnumProjects")) : nullptr;
    if (!proj || !enumProjects)
        return {};

    // REAPER lists open projects by index; find ours to learn its file name
    char fn[4096];
    auto check = [&](int idx) {
        fn[0] = 0;
        void* p = enumProjects(idx, fn, sizeof(fn));
        return !p ? -1 : p == proj ? 1 : 0;
    };
    if (check(kRenderingProject) == 1)
        return parentDir(fn);
    for (int i = 0;; ++i) {
        int r = check(i);
        if (r < 0)
            return {};
        if (r == 1)
            return parentDir(fn);
    }
}

std::string ReaperApi::resolveFile(const std::string& path, const std::string& subdir) const
{
    auto resolve = getFunc_ ? reinterpret_cast<ResolveFn>(getFunc_("resolve_fn2")) : nullptr;
    if (!resolve || path.empty())
        return path;
    char out[4096] = { 0 };
    resolve(path.c_str(), out, sizeof(out), subdir.empty() ? nullptr : subdir.c_str());
    return out[0] ? out : path;
}

intptr_t ReaperApi::fileCallback(void* userdata, int msg, void* parm)
{
    auto* reg = static_cast<Registered*>(userdata);
    ReaperApi* self = reg->api;
    switch (msg) {
    case 0: // copied or moved, e.g. by "copy all media into project directory"
        if (parm) {
            reg->file.path = static_cast<const char*>(parm);
            if (reg->main && self->onRename_)
                self->onRename_(self->context_, reg->file.path.c_str());
        }
        return 0;
    case 0x100:
        return reg->file.subdir.empty() ? 0 : (intptr_t)reg->file.subdir.c_str();
    case 0x101: {
        auto getContext = reinterpret_cast<GetContextFn>(self->getFunc_("clap_get_reaper_context"));
        return getContext ? (intptr_t)getContext(self->host_, kContextFxDsp) : 0;
    }
    case 0x103:
        return (intptr_t) "sfz123";
    default:
        return 0;
    }
}

void ReaperApi::unregisterAll()
{
    for (auto& r : registered_) {
        void* p[4] = { (void*)r->file.path.c_str(), registeredProject_, r.get(), (void*)&fileCallback };
        register_("-file_in_project_ex2", p);
    }
    registered_.clear();
    registeredProject_ = nullptr;
}

void ReaperApi::setProjectFiles(const std::vector<ProjectFile>& files)
{
    if (!register_)
        return;
    unregisterAll();

    void* proj = project();
    if (files.empty() || !proj)
        return;
    registeredProject_ = proj;
    for (size_t i = 0; i < files.size(); ++i) {
        registered_.push_back(std::unique_ptr<Registered>(new Registered { this, files[i], i == 0 }));
        Registered* r = registered_.back().get();
        void* p[4] = { (void*)r->file.path.c_str(), proj, r, (void*)&fileCallback };
        register_("file_in_project_ex2", p);
    }
}

std::string relativePath(const std::string& path, const std::string& baseDir)
{
    if (path.empty() || baseDir.empty() || path[0] != '/' || baseDir[0] != '/')
        return {};
    auto from = split(baseDir);
    auto to = split(path);
    size_t common = 0;
    while (common < from.size() && common < to.size() - 1 && from[common] == to[common])
        ++common;

    std::string rel;
    for (size_t i = common; i < from.size(); ++i)
        rel += "../";
    for (size_t i = common; i < to.size(); ++i)
        rel += to[i] + (i + 1 < to.size() ? "/" : "");
    return rel;
}

std::string joinPath(const std::string& dir, const std::string& rel)
{
    if (rel.empty() || rel[0] == '/')
        return rel;
    auto parts = split(dir);
    for (const auto& p : split(rel)) {
        if (p == "..") {
            if (!parts.empty())
                parts.pop_back();
        } else if (p != ".") {
            parts.push_back(p);
        }
    }
    std::string out;
    for (const auto& p : parts)
        out += "/" + p;
    return out;
}

bool fileExists(const std::string& path)
{
    struct stat st {};
    return !path.empty() && stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

} // namespace sfp
