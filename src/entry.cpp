// CLAP entry point and plugin factory.
#include "plugin.h"
#include "presets.h"

#include <cstring>

namespace {

uint32_t factoryCount(const clap_plugin_factory*) { return 1; }

const clap_plugin_descriptor* factoryDescriptor(const clap_plugin_factory*, uint32_t index)
{
    return index == 0 ? &sfp::kDescriptor : nullptr;
}

const clap_plugin* factoryCreate(const clap_plugin_factory*, const clap_host* host, const char* id)
{
    if (!clap_version_is_compatible(host->clap_version) || std::strcmp(id, sfp::kDescriptor.id) != 0)
        return nullptr;
    auto* p = new sfp::Plugin(host);
    return p->clapPlugin();
}

const clap_plugin_factory kFactory = { factoryCount, factoryDescriptor, factoryCreate };

bool entryInit(const char*) { return true; }
void entryDeinit() {}

const void* entryGetFactory(const char* id)
{
    if (!std::strcmp(id, CLAP_PLUGIN_FACTORY_ID))
        return &kFactory;
    if (!std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID) || !std::strcmp(id, CLAP_PRESET_DISCOVERY_FACTORY_ID_COMPAT))
        return &sfp::kPresetDiscoveryFactory;
    return nullptr;
}

} // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry clap_entry = { CLAP_VERSION_INIT, entryInit, entryDeinit, entryGetFactory };
