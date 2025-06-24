#pragma once

#include "core/Exception.h"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <unordered_map>

namespace sparkle
{
class ConfigValueBase
{
public:
    enum ConfigType : uint8_t
    {
        Bool,
        Int,
        Float,
        String,
        Count
    };

    ConfigValueBase(const char *name, const char *help, const char *category, ConfigType type, bool is_dynamic);

    ConfigValueBase(const ConfigValueBase &) = default;
    ConfigValueBase(ConfigValueBase &&) = default;
    ConfigValueBase &operator=(const ConfigValueBase &) = default;
    ConfigValueBase &operator=(ConfigValueBase &&) = default;

    virtual ~ConfigValueBase() = default;

    [[nodiscard]] std::string GetName() const
    {
        return name_;
    }

    [[nodiscard]] std::string GetHelp() const
    {
        return help_;
    }

    [[nodiscard]] std::string GetCategory() const
    {
        return category_;
    }

    [[nodiscard]] ConfigType GetType() const
    {
        return type_;
    }

    [[nodiscard]] bool IsLoaded() const
    {
        return is_loaded_;
    }

    [[nodiscard]] bool IsDynamic() const
    {
        return is_dynamic_;
    }

    template <class T> [[nodiscard]] T GetValueAs() const;

    template <class T> void SetValueAs(const T &value);

private:
    bool FromString(const std::string &value_string)
    {
        auto previous_value = ToString();
        if (FromStringInternal(value_string))
        {
            PrintLoading(previous_value);
            return true;
        }
        return false;
    }

    bool FromJson(const nlohmann::json &json_file)
    {
        auto previous_value = ToString();
        if (FromJsonInternal(json_file))
        {
            PrintLoading(previous_value);
            return true;
        }
        return false;
    }

    void PrintLoading(const std::string &previous_value) const;

    virtual bool FromStringInternal(const std::string &value_string) = 0;

    virtual bool FromJsonInternal(const nlohmann::json &json_file) = 0;

    virtual void ToJson(nlohmann::json &json_file) = 0;

    [[nodiscard]] virtual std::string ToString() const = 0;

    std::string name_;
    std::string help_;
    std::string category_;
    bool is_dynamic_;
    ConfigType type_;

protected:
    bool is_loaded_ = false;

    friend class ConfigManager;
};

template <typename T>
concept AllowedConfigType = std::is_same_v<bool, T> || std::is_same_v<uint32_t, T> || std::is_same_v<float, T> ||
                            std::is_same_v<std::string, T>;

template <AllowedConfigType T> class ConfigValue final : public ConfigValueBase
{
public:
    ConfigValue(const char *name, const char *help, const char *category, const T &default_value,
                bool is_dynamic = false);

    [[nodiscard]] T Get() const
    {
        return value_;
    }

    void Set(T new_value);

    static ConfigType GetType();

    void SetOnChangeCallback(std::function<void(ConfigValue<T> &)> callback)
    {
        on_change_callback_ = std::move(callback);
    }

private:
    bool FromStringInternal(const std::string &value_string) override;

    bool FromJsonInternal(const nlohmann::json &json_file) override;

    [[nodiscard]] std::string ToString() const override;

    static auto &GetRegistration()
    {
        static std::unordered_map<std::string, ConfigValue<T> *> registered_configs;
        return registered_configs;
    }

    void ToJson(nlohmann::json &json_file) override;

    T value_;

    std::function<void(ConfigValue<T> &)> on_change_callback_;

    friend class ConfigManager;
};

template <class T> T ConfigValueBase::GetValueAs() const
{
    ASSERT_EQUAL(type_, ConfigValue<T>::GetType());
    return static_cast<const ConfigValue<T> *>(this)->Get();
}

template <class T> void ConfigValueBase::SetValueAs(const T &value)
{
    ASSERT_EQUAL(type_, ConfigValue<T>::GetType());
    static_cast<ConfigValue<T> *>(this)->Set(value);
}

#ifndef IMPLEMENT_CONFIG_VALUE
#define IMPLEMENT_CONFIG_VALUE 0
#endif

#if !IMPLEMENT_CONFIG_VALUE
extern template class ConfigValue<uint32_t>;
extern template class ConfigValue<bool>;
extern template class ConfigValue<float>;
extern template class ConfigValue<std::string>;
#endif
} // namespace sparkle
