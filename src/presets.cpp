#include "presets.h"

#include "plugin.h"
#include "prefs.h"
#include "reaper.h"

#include <cstring>
#include <map>

namespace sfp {

namespace {

// REAPER only creates providers whose id is the plug-in id
const clap_preset_discovery_provider_descriptor kProviderDesc = {
    CLAP_VERSION_INIT,
    kDescriptor.id,
    "SF Player recent instruments",
    "Blake Oliver",
};

std::string fileTitle(const std::string& path)
{
    std::string name = path.substr(path.find_last_of('/') + 1);
    auto dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string folderName(const std::string& path)
{
    std::string dir = path.substr(0, path.find_last_of('/'));
    return dir.substr(dir.find_last_of('/') + 1);
}

struct Provider {
    clap_preset_discovery_provider clap;
    const clap_preset_discovery_indexer* indexer;
};

bool providerInit(const clap_preset_discovery_provider* p)
{
    auto* indexer = static_cast<Provider*>(p->provider_data)->indexer;
    static const clap_preset_discovery_filetype kTypes[] = {
        { "SoundFont 2", "SF2 SoundFont", "sf2" },
        { "SoundFont 3", "SF3 SoundFont with compressed samples", "sf3" },
        { "SFZ", "SFZ instrument", "sfz" },
    };
    for (const auto& t : kTypes)
        indexer->declare_filetype(indexer, &t);
    const clap_preset_discovery_location recent { CLAP_PRESET_DISCOVERY_IS_USER_CONTENT, "Recent instruments",
        CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr };
    indexer->declare_location(indexer, &recent);
    return true;
}

void providerDestroy(const clap_preset_discovery_provider* p)
{
    delete static_cast<Provider*>(p->provider_data);
}

// one preset per recent instrument; the load key is its path
bool providerGetMetadata(const clap_preset_discovery_provider*, uint32_t kind, const char*,
    const clap_preset_discovery_metadata_receiver* r)
{
    if (kind != CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN)
        return false;
    std::vector<std::string> files;
    std::map<std::string, int> titles;
    for (const auto& f : recentInstruments())
        if (fileExists(f)) {
            files.push_back(f);
            ++titles[fileTitle(f)];
        }

    const clap_universal_plugin_id id { "clap", kDescriptor.id };
    for (const auto& f : files) {
        std::string name = fileTitle(f);
        if (titles[name] > 1)
            name += " (" + folderName(f) + ")";
        if (!r->begin_preset(r, name.c_str(), f.c_str()))
            break;
        r->add_plugin_id(r, &id);
        r->set_flags(r, CLAP_PRESET_DISCOVERY_IS_USER_CONTENT);
        r->set_description(r, f.c_str());
        r->add_feature(r, kindForPath(f) == InstrumentKind::Sfz ? "sfz" : "soundfont");
    }
    return true;
}

const void* providerGetExtension(const clap_preset_discovery_provider*, const char*) { return nullptr; }

uint32_t factoryCount(const clap_preset_discovery_factory*) { return 1; }

const clap_preset_discovery_provider_descriptor* factoryDescriptor(const clap_preset_discovery_factory*, uint32_t i)
{
    return i == 0 ? &kProviderDesc : nullptr;
}

const clap_preset_discovery_provider* factoryCreate(const clap_preset_discovery_factory*,
    const clap_preset_discovery_indexer* indexer, const char* id)
{
    if (std::strcmp(id, kProviderDesc.id) != 0)
        return nullptr;
    auto* p = new Provider {};
    p->indexer = indexer;
    p->clap = { &kProviderDesc, p, providerInit, providerDestroy, providerGetMetadata, providerGetExtension };
    return &p->clap;
}

} // namespace

const clap_preset_discovery_factory kPresetDiscoveryFactory = { factoryCount, factoryDescriptor, factoryCreate };

} // namespace sfp
