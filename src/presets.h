// CLAP preset discovery, so the host's own preset menu (REAPER's FX window preset list) offers
// recent instruments, and the host can import .sf2, .sf3 and .sfz files as presets.
//
// REAPER indexes presets once per session and shares them between instances, and only reads
// presets the plug-in lists itself (location kind PLUGIN), so the list cannot follow the
// instrument loaded in each instance.
#pragma once

#include <clap/clap.h>

namespace sfp {

extern const clap_preset_discovery_factory kPresetDiscoveryFactory;

} // namespace sfp
