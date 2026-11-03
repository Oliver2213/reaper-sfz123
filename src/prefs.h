// User preferences shared by every SF Player instance.
#pragma once

#include <string>
#include <vector>

namespace sfp {

std::vector<std::string> recentInstruments(); // newest first
void addRecentInstrument(const std::string& path);

// rename the FX in REAPER after the loaded instrument (on by default)
bool nameFxAfterInstrument();
void setNameFxAfterInstrument(bool on);

} // namespace sfp
