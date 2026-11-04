// Optional REAPER integration through the "cockos.reaper_extension" CLAP host extension.
// In other hosts every call is a no-op.
#pragma once

#include <clap/clap.h>

#include <memory>
#include <string>
#include <vector>

namespace sfp {

class ReaperApi {
public:
    using RenameCallback = void (*)(void* context, const char* newPath);

    struct ProjectFile {
        std::string path;
        std::string subdir; // where REAPER should put the copy, relative to the project's media folder
    };

    ~ReaperApi() { setProjectFiles({}); }

    void init(const clap_host* host, RenameCallback onRename, void* context);
    bool available() const { return getFunc_ != nullptr; }

    // Folder of the project this plug-in instance belongs to; empty if unsaved or not in REAPER.
    std::string projectDirectory() const;

    // Finds a file the way REAPER finds moved media (project folder, media paths, `subdir`);
    // returns `path` unchanged if REAPER can't find it either.
    std::string resolveFile(const std::string& path, const std::string& subdir) const;

    // Tells REAPER the project uses these files, so "copy all media into project directory", the
    // project bay and "clean project directory" include them. When REAPER copies or moves files[0]
    // the rename callback reports its new path. An empty list clears the registration.
    void setProjectFiles(const std::vector<ProjectFile>& files);

    // The name the user gave this FX in REAPER ("renamed_name"; empty when not renamed). Returns
    // false when it can't be read, e.g. outside REAPER or in an FX container.
    bool fxRename(std::string& name) const;
    bool setFxRename(const std::string& name);

    // Name of the track this FX is on. Fails for take FX, and for any but the first sfz123 on
    // the track, so two instances don't fight over the name.
    bool trackName(std::string& name) const;
    bool setTrackName(const std::string& name);

private:
    struct Registered {
        ReaperApi* api;
        ProjectFile file;
        bool main;
    };
    static intptr_t fileCallback(void* self, int msg, void* parm);
    void* project() const;
    void* context(int sel) const;
    // calls fn(TrackFX_/TakeFX_ named config function, track or take, fx index) for our FX
    template <typename Fn> bool withFx(const char* api, Fn fn) const;
    void* firstTrackOfOurs() const;
    void unregisterAll();

    const clap_host* host_ = nullptr;
    int (*register_)(const char* name, void* info) = nullptr;
    void* (*getFunc_)(const char* name) = nullptr;
    RenameCallback onRename_ = nullptr;
    void* context_ = nullptr;
    std::vector<std::unique_ptr<Registered>> registered_;
    void* registeredProject_ = nullptr;
};

// Path helpers for project-relative instrument paths.
std::string relativePath(const std::string& path, const std::string& baseDir);
std::string joinPath(const std::string& dir, const std::string& rel);
bool fileExists(const std::string& path);

} // namespace sfp
