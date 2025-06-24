#define IMPLEMENT_CONFIG_VALUE 1

#include "core/ConfigValue.h"

#include "core/ConfigManager.h"
#include "core/Logger.h"

#include <charconv>
#include <fast_float/fast_float.h>
#include <nlohmann/json.hpp>

namespace sparkle
{
ConfigValueBase::ConfigValueBase(const char *name, const char *help, const char *category, ConfigType type,
                                 bool dynamic)
    : name_(name), help_(help), category_(category), is_dynamic_(dynamic), type_(type)
{
    ConfigManager::Instance().Register(*this);
}

void ConfigValueBase::PrintLoading(const std::string &previous_value) const
{
    Log(Info, "Loaded config {}: {}->{}", GetName(), previous_value, ToString());
}

template <AllowedConfigType T>
ConfigValue<T>::ConfigValue(const char *name, const char *help, const char *category, const T &default_value,
                            bool dynamic)
    : ConfigValueBase(name, help, category, GetType(), dynamic), value_(default_value)
{
    GetRegistration().insert_or_assign(name, this);

    if (ConfigManager::Instance().IsInitialized())
    {
        // if we have missed ConfigManager::LoadAll(), load this config manually.
        // this can happen if the module is dynamically loaded after the program initialization

        ConfigManager::Instance().Load(*this);
    }
}

template <AllowedConfigType T> bool ConfigValue<T>::FromJsonInternal(const nlohmann::json &json_file)
{
    if (!json_file.contains(GetName()))
    {
        return false;
    }

    value_ = json_file.at(GetName());
    is_loaded_ = true;

    return true;
}

template <AllowedConfigType T> void ConfigValue<T>::Set(T new_value)
{
    Log(Info, "runtime config change. {}: {}->{}", GetName(), value_, new_value);

    value_ = new_value;

    if (on_change_callback_)
    {
        on_change_callback_(*this);
    }
}

template <AllowedConfigType T> void ConfigValue<T>::ToJson(nlohmann::json &json_file)
{
    json_file[GetName()] = value_;
}

template <> bool ConfigValue<bool>::FromStringInternal(const std::string &value_string)
{
    if (value_string == "true" || value_string == "True" || value_string == "TRUE" || value_string == "1")
    {
        value_ = true;
    }
    else if (value_string == "false" || value_string == "False" || value_string == "FALSE" || value_string == "1")
    {
        value_ = false;
    }
    else
    {
        Log(Warn, "Argument {}={} parse failed. Not a boolean", GetName(), value_string);
        return false;
    }

    is_loaded_ = true;

    return true;
}

template <> bool ConfigValue<uint32_t>::FromStringInternal(const std::string &value_string)
{
    uint32_t parsed_value = 0;
    auto result = std::from_chars(value_string.data(), value_string.data() + value_string.size(), parsed_value);

    if (result.ec != std::errc{})
    {
        Log(Warn, "Argument {}={} parse failed. err: {}", GetName(), value_string, static_cast<int>(result.ec));
        return false;
    }

    value_ = parsed_value;
    is_loaded_ = true;

    return true;
}

template <> bool ConfigValue<float>::FromStringInternal(const std::string &value_string)
{
    float parsed_value = 0;
    auto result = fast_float::from_chars(value_string.data(), value_string.data() + value_string.size(), parsed_value);

    if (result.ec != std::errc{})
    {
        Log(Warn, "Argument {}={} parse failed. err: {}", GetName(), value_string, static_cast<int>(result.ec));
        return false;
    }

    value_ = parsed_value;
    is_loaded_ = true;

    return true;
}

template <> bool ConfigValue<std::string>::FromStringInternal(const std::string &value_string)
{
    value_ = value_string;
    is_loaded_ = true;

    return true;
}

template <> std::string ConfigValue<bool>::ToString() const
{
    return value_ ? "true" : "false";
}

template <> std::string ConfigValue<uint32_t>::ToString() const
{
    return std::to_string(value_);
}

template <> std::string ConfigValue<float>::ToString() const
{
    return std::to_string(value_);
}

template <> std::string ConfigValue<std::string>::ToString() const
{
    return value_;
}

template <> ConfigValueBase::ConfigType ConfigValue<std::string>::GetType()
{
    return ConfigType::String;
}

template <> ConfigValueBase::ConfigType ConfigValue<float>::GetType()
{
    return ConfigType::Float;
}

template <> ConfigValueBase::ConfigType ConfigValue<uint32_t>::GetType()
{
    return ConfigType::Int;
}

template <> ConfigValueBase::ConfigType ConfigValue<bool>::GetType()
{
    return ConfigType::Bool;
}

template class ConfigValue<uint32_t>;
template class ConfigValue<bool>;
template class ConfigValue<float>;
template class ConfigValue<std::string>;
} // namespace sparkle
