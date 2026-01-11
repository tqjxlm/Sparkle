#pragma once

#include "core/ConfigValue.h"

#include <unordered_map>

namespace argparse
{
class ArgumentParser;
};

namespace sparkle
{
class UiManager;
struct ConfigCollection;

class ConfigManager
{
public:
    /*
        Config hierarchy (larger number means higher priority):
        1. packed (built-in) config
        2. user (generated) config
        3. runtime argument (which may not be available on mobile platforms)
    */
    ConfigManager();

    ~ConfigManager();

    // this can be slow. avoid intensive use.
    template <class T> ConfigValue<T> *GetConfig(const std::string &config_name)
    {
        if (!registered_configs_.contains(config_name))
        {
            return nullptr;
        }

        // it is guaranteed to exist
        auto *config = ConfigValue<T>::GetRegistration()[config_name];

        ASSERT(config);
        ASSERT_EQUAL(config->GetType(), ConfigValue<T>::GetType());

        return config;
    }

    // CAUTION: not thread-safe
    static ConfigManager &Instance()
    {
        // singleton: it's not ideal practice but we want to support anywhere auto-registration of ConfigValue
        static ConfigManager instance;
        return instance;
    }

    void SetArgs(int argc, const char *const argv[]);

    void SaveAll();

    void LoadAll();

    void Load(ConfigValueBase &config);

    [[nodiscard]] bool IsInitialized() const
    {
        return initialized_;
    }

    [[nodiscard]] const auto &GetConfigsInCategories() const
    {
        return registered_categories_;
    }

    static void DrawUi(UiManager *ui_manager, const std::vector<std::pair<const char *, ConfigCollection *>> &configs);

private:
    void LoadFromArgs();

    void LoadFromFile(bool generated);

    void Register(ConfigValueBase &config);

    bool initialized_ = false;

    std::unordered_map<std::string, ConfigValueBase &> registered_configs_;

    std::unordered_map<std::string, std::vector<ConfigValueBase *>> registered_categories_;

    int argc_ = 0;

    std::vector<const char *> argv_;

    std::unique_ptr<argparse::ArgumentParser> argparser_;

    std::unique_ptr<nlohmann::json> json_file_;

    friend ConfigValueBase;
};
} // namespace sparkle
