// Optional REAPER integration through the "cockos.reaper_extension" CLAP host extension.
// In other hosts every call is a no-op.
#pragma once

#include <clap/clap.h>

#include <string>

namespace sfp {

class ReaperApi {
public:
    using RenameCallback = void (*)(void* context, const char* newPath);

    ~ReaperApi() { setProjectFile({}); }

    void init(const clap_host* host, RenameCallback onRename, void* context);

    // Folder of the project this plug-in instance belongs to; empty if unsaved or not in REAPER.
    std::string projectDirectory() const;

    // Finds a file the way REAPER finds moved media (project folder, media paths); returns `path`
    // unchanged if REAPER can't find it either.
    std::string resolveFile(const std::string& path) const;

    // Tells REAPER the project uses this file, so "copy media" and "clean project directory"
    // include it. REAPER reports renames through the callback. Empty clears the registration.
    void setProjectFile(const std::string& path);

private:
    static intptr_t fileCallback(void* self, int msg, void* parm);
    void* project() const;

    const clap_host* host_ = nullptr;
    int (*register_)(const char* name, void* info) = nullptr;
    void* (*getFunc_)(const char* name) = nullptr;
    RenameCallback onRename_ = nullptr;
    void* context_ = nullptr;
    std::string registeredFile_;
    void* registeredProject_ = nullptr;
};

// Path helpers for project-relative instrument paths.
std::string relativePath(const std::string& path, const std::string& baseDir);
std::string joinPath(const std::string& dir, const std::string& rel);
bool fileExists(const std::string& path);

} // namespace sfp
