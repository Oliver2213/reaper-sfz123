// Recently loaded instruments, shared by every SF Player instance.
#pragma once

#include <string>
#include <vector>

namespace sfp {

std::vector<std::string> recentInstruments(); // newest first
void addRecentInstrument(const std::string& path);

} // namespace sfp
