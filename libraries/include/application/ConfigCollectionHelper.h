#pragma once

#include "application/ConfigCollection.h"
#include "core/Enum.h"
#include "core/GuiHelper.h"
#include "core/Logger.h"
#include "core/math/Utilities.h"

namespace sparkle
{
struct ConfigCollectionHelper
{
    template <class T>
    static void LogConfigChange(const std::string &config_name, const T &old_value, const T &new_value)
    {
        Log(Info, "on config change. {}: {}->{}.", config_name, old_value, new_value);
    }

    template <EnumType T> static void AddConfigToUi(ConfigValue<std::string> &config)
    {
        // only dynamic config can be changed at runtime
        if (!config.IsDynamic())
        {
            return;
        }

        auto old_value = config.Get();
        if (ImGui::BeginCombo(gui_helper::LabelPrefix(config.GetName()).c_str(), old_value.c_str()))
        {
            constexpr auto OutputModes = magic_enum::enum_entries<T>();
            for (auto mode : OutputModes)
            {
                bool is_selected = old_value == mode.second;
                if (ImGui::Selectable(mode.second.data(), is_selected))
                {
                    std::string new_value = {mode.second.begin(), mode.second.end()};
                    config.SetValueAs(new_value);
                }

                if (is_selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndCombo();
        }
    }

    static void AddConfigToUi(ConfigValueBase &config)
    {
        // only dynamic config can be changed at runtime
        if (!config.IsDynamic())
        {
            return;
        }

        switch (config.GetType())
        {
        case ConfigValueBase::Bool: {
            bool on = config.GetValueAs<bool>();

            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);

            if (ImGui::Checkbox(gui_helper::LabelPrefix(config.GetName()).c_str(), &on))
            {
                if (on != config.GetValueAs<bool>())
                {
                    config.SetValueAs(on);
                }
            }
            ImGui::PopStyleVar();
        }
        break;
        default:
            break;
        }
    }

    template <class T> static void RegisterConfig(ConfigCollection *collection, ConfigValue<T> &config, T &value)
    {
        value = config.Get();

        config.SetOnChangeCallback([&value, collection](ConfigValue<T> &cfg) {
            if (utilities::IsSame(cfg.Get(), value))
            {
                return;
            }

            LogConfigChange(cfg.GetName(), value, cfg.Get());

            value = cfg.Get();

            collection->Validate();
        });

        collection->registered_configs_.push_back(&config);

        collection->config_ui_generators_.emplace_back([&config]() { AddConfigToUi(config); });
    }

    template <EnumType T>
    static void RegisterConfig(ConfigCollection *collection, ConfigValue<std::string> &config, T &value)
    {
        if (!Str2Enum(config.Get(), value))
        {
            return;
        }

        config.SetOnChangeCallback([&value, collection](ConfigValue<std::string> &cfg) {
            T new_value;

            if (!Str2Enum(cfg.Get(), new_value))
            {
                Log(Error, "invalid enum value. {}: {}.", cfg.GetName(), cfg.Get());
                return;
            }

            if (new_value == value)
            {
                return;
            }

            LogConfigChange(cfg.GetName(), Enum2Str(value), cfg.Get().c_str());

            value = new_value;

            collection->Validate();
        });

        collection->registered_enum_configs_.emplace_back(&config);

        collection->config_ui_generators_.emplace_back([&config]() { AddConfigToUi<T>(config); });
    }
};
} // namespace sparkle
