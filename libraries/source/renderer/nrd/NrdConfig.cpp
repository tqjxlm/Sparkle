#include "renderer/nrd/NrdConfig.h"

#include "application/ConfigCollectionHelper.h"

namespace sparkle
{
static ConfigValue<bool> config_nrd_stabilization(
    "nrd_stabilization", "ReBLUR temporal stabilization pass (off saves one full-res pass; the resolve EMA remains)",
    "nrd", true, true);
static ConfigValue<std::string> config_nrd_debug("nrd_debug", "NRD channel to visualize", "nrd",
                                                 Enum2Str<NrdDebugMode::None>(), true);

NrdConfig &NrdConfig::Get()
{
    static NrdConfig instance;
    [[maybe_unused]] static const bool Initialized = (instance.Init(), true);
    return instance;
}

void NrdConfig::Init()
{
    ConfigCollectionHelper::RegisterConfig(this, config_nrd_stabilization, stabilization);
    ConfigCollectionHelper::RegisterConfig(this, config_nrd_debug, debug_mode);
}
} // namespace sparkle
