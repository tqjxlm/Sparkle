#pragma once

#include "core/ConfigValue.h"

namespace sparkle
{
struct ConfigCollection
{
public:
    ConfigCollection() = default;
    ConfigCollection(const ConfigCollection &other) = default;
    ConfigCollection &operator=(const ConfigCollection &other) = default;
    virtual ~ConfigCollection() = default;

    virtual void Validate() = 0;

    [[nodiscard]] const auto &GetConfigUiGenerators() const
    {
        return config_ui_generators_;
    }

    // custom widgets (beyond the auto-generated per-config ones) drawn in this collection's panel tab
    void AddUiGenerator(std::function<void(void)> generator)
    {
        config_ui_generators_.emplace_back(std::move(generator));
    }

private:
    std::vector<ConfigValueBase *> registered_configs_;
    std::vector<ConfigValue<std::string> *> registered_enum_configs_;
    std::vector<std::function<void(void)>> config_ui_generators_;

    friend struct ConfigCollectionHelper;
};
} // namespace sparkle
