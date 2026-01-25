#include "core/ConfigManager.h"

#include "application/ConfigCollection.h"
#include "core/FileManager.h"
#include "core/Logger.h"

#include <argparse/argparse.hpp>
#include <imgui.h>
#include <nlohmann/json.hpp>

namespace sparkle
{
static const char *config_path = "config/config.json";

ConfigManager::ConfigManager() = default;

ConfigManager::~ConfigManager() = default;

void ConfigManager::SetArgs(int argc, const char *const argv[])
{
    argc_ = argc;
    for (auto i = 0u; i < static_cast<unsigned>(argc_); i++)
    {
        argv_.emplace_back(argv[i]);
    }
}

static std::string AsArgumentName(const std::string &name)
{
    return std::string("--") + name;
}

void ConfigManager::LoadFromFile(bool generated)
{
    using json = nlohmann::json;

    const auto &raw_data = generated ? FileManager::GetNativeFileManager()->Read(Path::External(config_path))
                                     : FileManager::GetNativeFileManager()->Read(Path::Resource(config_path));

    json data = json::parse(raw_data, nullptr, false);
    if (data.empty() || data.is_discarded())
    {
        Log(Info, "Config file parse failed. Will not use {} config file.", generated ? "runtime" : "packaged");
        return;
    }

    Log(Info, "Parsing configs from config file: {}", generated ? "runtime" : "packaged");

    json_file_ = std::make_unique<json>(data);

    for (auto &[name, config] : registered_configs_)
    {
        config.FromJson(data);
    }
}

void ConfigManager::LoadFromArgs()
{
    if (argc_ == 0)
    {
        return;
    }

    Log(Info, "Parsing configs from args");

    argparser_ = std::make_unique<argparse::ArgumentParser>("");

    for (auto &[name, config] : registered_configs_)
    {
        argparser_->add_argument(AsArgumentName(config.GetName())).help(config.GetHelp());
    }

    // Log(Info, "{}", argparser_->help().str());

    argparser_->parse_known_args(argc_, argv_.data());

    for (auto &[name, config] : registered_configs_)
    {
        const auto &value_string = argparser_->present(AsArgumentName(config.GetName()));
        if (value_string)
        {
            config.FromString(value_string.value());
        }
    }
}

void ConfigManager::SaveAll()
{
    using json = nlohmann::json;

    json data_to_write;

    for (auto &[name, config] : registered_configs_)
    {
        config.ToJson(data_to_write);
    }

    const auto &raw_data = data_to_write.dump(4);

    const auto &write_result =
        FileManager::GetNativeFileManager()->Write(Path::External(config_path), raw_data.data(), raw_data.size());

    if (!write_result.empty())
    {
        Log(Info, "Config file saved to {}.", write_result);
    }
    else
    {
        Log(Error, "Failed to save config file.");
    }
}

void ConfigManager::LoadAll()
{
    // later phases have priority and overwrite previous ones

    LoadFromFile(false);

    LoadFromFile(true);

    LoadFromArgs();

    initialized_ = true;
}

void ConfigManager::Load(ConfigValueBase &config)
{
    if (json_file_)
    {
        config.FromJson(*json_file_);
    }

    if (argparser_)
    {
        const auto &value_string = argparser_->present(AsArgumentName(config.GetName()));
        if (value_string)
        {
            config.FromString(value_string.value());
        }
    }
}

void ConfigManager::Register(ConfigValueBase &config)
{
    if (registered_configs_.contains(config.GetName()))
    {
        // we may not have a logger ready at this point. do not use logger.
        printf("[FATAL] config name conflict: %s. choose another one", config.GetName().c_str());
        DumpAndAbort();
    }

    registered_configs_.insert_or_assign(config.GetName(), config);

    auto found_category = registered_categories_.find(config.GetCategory());
    if (found_category == registered_categories_.end())
    {
        registered_categories_.insert({config.GetCategory(), {&config}});
    }
    else
    {
        found_category->second.push_back(&config);
    }
}

void ConfigManager::DrawUi(const std::vector<std::pair<const char *, ConfigCollection *>> &configs)
{
    ImGui::BeginTabBar("ConfigTabs", 0);

    for (const auto &config_collection : configs)
    {
        if (ImGui::BeginTabItem(config_collection.first))
        {
            for (const auto &generator : config_collection.second->GetConfigUiGenerators())
            {
                generator();
            }

            ImGui::EndTabItem();
        }
    }

    ImGui::EndTabBar();
}
} // namespace sparkle
