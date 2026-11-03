#include "reaper.h"

#include <cstdint>
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

constexpr int kContextProject = 3;
constexpr int kContextFxDsp = 4;
constexpr int kRenderingProject = 0x40000000;

std::string parentDir(const std::string& path)
{
    auto slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
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

void* ReaperApi::project() const
{
    auto getContext = getFunc_ ? reinterpret_cast<GetContextFn>(getFunc_("clap_get_reaper_context")) : nullptr;
    return getContext ? getContext(host_, kContextProject) : nullptr;
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
        return (intptr_t) "SF Player";
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
