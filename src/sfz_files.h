// Lists the files an SFZ instrument uses, so REAPER can copy the whole instrument with a project.
#pragma once

#include <string>
#include <vector>

namespace sfp {

// The .sfz itself, every #include'd file, and every sample that exists on disk (absolute paths).
std::vector<std::string> collectSfzFiles(const std::string& sfzPath);

// Deepest folder containing all of `files`.
std::string commonDirectory(const std::vector<std::string>& files);

} // namespace sfp
