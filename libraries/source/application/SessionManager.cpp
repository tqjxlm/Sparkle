#include "application/SessionManager.h"

#include "core/ConfigManager.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "core/math/Utilities.h"
#include "scene/Scene.h"
#include "scene/SceneManager.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/camera/OrbitCameraComponent.h"

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <string>

namespace sparkle
{
namespace
{
constexpr const char *SessionFilePath = "session/last_session.json";
constexpr const char *SessionConfigKey = "config";
constexpr const char *SessionCameraKey = "camera";
constexpr const char *SessionVersionKey = "version";
constexpr int SessionVersion = 1;

bool ReadVector3(const nlohmann::json &value, Vector3 &out)
{
    if (!value.is_array() || value.size() != 3)
    {
        return false;
    }

    if (!value[0].is_number() || !value[1].is_number() || !value[2].is_number())
    {
        return false;
    }

    out = Vector3(value[0].get<float>(), value[1].get<float>(), value[2].get<float>());
    return true;
}

bool ReadVector4(const nlohmann::json &value, Vector4 &out)
{
    if (!value.is_array() || value.size() != 4)
    {
        return false;
    }

    if (!value[0].is_number() || !value[1].is_number() || !value[2].is_number() || !value[3].is_number())
    {
        return false;
    }

    out = Vector4(value[0].get<float>(), value[1].get<float>(), value[2].get<float>(), value[3].get<float>());
    return true;
}

nlohmann::json BuildConfigJson()
{
    nlohmann::json config_json = nlohmann::json::object();

    const auto &categories = ConfigManager::Instance().GetConfigsInCategories();
    for (const auto &category_entry : categories)
    {
        for (auto *config : category_entry.second)
        {
            const auto name = config->GetName();
            switch (config->GetType())
            {
            case ConfigValueBase::Bool:
                config_json[name] = config->GetValueAs<bool>();
                break;
            case ConfigValueBase::Int:
                config_json[name] = config->GetValueAs<uint32_t>();
                break;
            case ConfigValueBase::Float:
                config_json[name] = config->GetValueAs<float>();
                break;
            case ConfigValueBase::String:
                config_json[name] = config->GetValueAs<std::string>();
                break;
            default:
                break;
            }
        }
    }

    return config_json;
}

std::string GetSceneConfigValue()
{
    auto *scene_config = ConfigManager::Instance().GetConfig<std::string>("scene");
    if (!scene_config)
    {
        return {};
    }

    return scene_config->Get();
}

nlohmann::json BuildCameraJson(const CameraComponent *camera)
{
    const auto &transform = camera->GetLocalTransform();
    const auto &translation = transform.GetTranslation();
    const auto &rotation = transform.GetRotation();
    const auto &scale = transform.GetScale();

    nlohmann::json camera_json = nlohmann::json::object();
    camera_json["translation"] = {translation.x(), translation.y(), translation.z()};
    camera_json["rotation"] = {rotation.x(), rotation.y(), rotation.z(), rotation.w()};
    camera_json["scale"] = {scale.x(), scale.y(), scale.z()};

    return camera_json;
}

void ApplyConfigJson(const nlohmann::json &config_json)
{
    const auto &categories = ConfigManager::Instance().GetConfigsInCategories();
    for (const auto &category_entry : categories)
    {
        for (auto *config : category_entry.second)
        {
            const auto name = config->GetName();
            auto value_it = config_json.find(name);
            if (value_it == config_json.end())
            {
                continue;
            }

            const auto &value = *value_it;
            switch (config->GetType())
            {
            case ConfigValueBase::Bool: {
                bool new_value = false;
                if (value.is_boolean())
                {
                    new_value = value.get<bool>();
                }
                else if (value.is_number_integer() || value.is_number_unsigned())
                {
                    new_value = value.get<int64_t>() != 0;
                }
                else
                {
                    Log(Warn, "Session config {} type mismatch (expected bool).", name);
                    continue;
                }

                if (config->GetValueAs<bool>() != new_value)
                {
                    Log(Debug, "Session config override {}: {}->{}", name, config->GetValueAs<bool>(), new_value);
                    config->SetValueAs<bool>(new_value);
                }
            }
            break;
            case ConfigValueBase::Int: {
                if (!value.is_number())
                {
                    Log(Warn, "Session config {} type mismatch (expected int).", name);
                    continue;
                }
                const auto new_value = static_cast<uint32_t>(value.get<double>());
                if (config->GetValueAs<uint32_t>() != new_value)
                {
                    Log(Debug, "Session config override {}: {}->{}", name, config->GetValueAs<uint32_t>(), new_value);
                    config->SetValueAs<uint32_t>(new_value);
                }
            }
            break;
            case ConfigValueBase::Float: {
                if (!value.is_number())
                {
                    Log(Warn, "Session config {} type mismatch (expected float).", name);
                    continue;
                }
                const auto new_value = value.get<float>();
                Log(Debug, "Session config restore {}: {}", name, new_value);
                config->SetValueAs<float>(new_value);
            }
            break;
            case ConfigValueBase::String: {
                if (!value.is_string())
                {
                    Log(Warn, "Session config {} type mismatch (expected string).", name);
                    continue;
                }
                const auto new_value = value.get<std::string>();
                if (config->GetValueAs<std::string>() != new_value)
                {
                    Log(Debug, "Session config override {}: {}->{}", name, config->GetValueAs<std::string>(),
                        new_value);
                    config->SetValueAs<std::string>(new_value);
                }
            }
            break;
            default:
                break;
            }
        }
    }
}

std::optional<SessionManager::CameraState> ParseCameraState(const nlohmann::json &camera_json)
{
    if (!camera_json.is_object())
    {
        return std::nullopt;
    }

    SessionManager::CameraState state;

    auto translation_it = camera_json.find("translation");
    if (translation_it == camera_json.end() || !ReadVector3(*translation_it, state.translation))
    {
        return std::nullopt;
    }

    auto rotation_it = camera_json.find("rotation");
    if (rotation_it == camera_json.end() || !ReadVector4(*rotation_it, state.rotation))
    {
        return std::nullopt;
    }

    auto scale_it = camera_json.find("scale");
    if (scale_it != camera_json.end())
    {
        if (!ReadVector3(*scale_it, state.scale))
        {
            return std::nullopt;
        }
    }

    return state;
}

bool ReadSessionJson(nlohmann::json &out)
{
    auto *file_manager = FileManager::GetNativeFileManager();
    const Path session_path = Path::External(SessionFilePath);
    if (!file_manager->Exists(session_path))
    {
        return false;
    }

    auto raw_data = file_manager->Read(session_path);
    if (raw_data.empty())
    {
        return false;
    }

    out = nlohmann::json::parse(raw_data, nullptr, false);
    return !out.is_discarded() && out.is_object();
}

bool WriteSessionJson(const nlohmann::json &session_json)
{
    auto *file_manager = FileManager::GetNativeFileManager();
    const auto raw_data = session_json.dump(4);
    const auto &write_result = file_manager->Write(Path::External(SessionFilePath), raw_data.data(), raw_data.size());
    if (write_result.empty())
    {
        return false;
    }

    Log(Info, "Session saved to {}.", write_result);
    return true;
}
} // namespace

void SessionManager::SetLoadLastSession(bool load_last_session)
{
    load_last_session_ = load_last_session;
}

bool SessionManager::LoadLastSessionInternal()
{
    nlohmann::json session_json;
    if (!ReadSessionJson(session_json))
    {
        Log(Info, "No valid session was found.");
        return false;
    }

    if (auto version_it = session_json.find(SessionVersionKey); version_it != session_json.end())
    {
        if (!version_it->is_number_integer() || version_it->get<int>() != SessionVersion)
        {
            Log(Warn, "Unsupported session version.");
            return false;
        }
    }

    auto config_it = session_json.find(SessionConfigKey);
    if (config_it != session_json.end() && config_it->is_object())
    {
        ApplyConfigJson(*config_it);
        Log(Info, "Session config restored.");
    }
    else
    {
        Log(Warn, "Session config missing or invalid.");
    }

    auto camera_it = session_json.find(SessionCameraKey);
    if (camera_it != session_json.end())
    {
        pending_camera_ = ParseCameraState(*camera_it);
        if (!pending_camera_)
        {
            Log(Warn, "Session camera data invalid.");
        }
    }

    return true;
}

bool SessionManager::LoadLastSession()
{
    return LoadLastSessionInternal();
}

void SessionManager::LoadLastSessionIfRequested()
{
    if (!load_last_session_)
    {
        return;
    }

    LoadLastSessionInternal();
}

void SessionManager::ApplyCamera(CameraComponent *camera)
{
    if (!pending_camera_ || !camera)
    {
        return;
    }

    const auto &state = *pending_camera_;
    const Rotation rotation = utilities::Vector4AsQuaternion(state.rotation);
    camera->GetNode()->SetTransform(state.translation, rotation, state.scale);

    if (auto *orbit_camera = dynamic_cast<OrbitCameraComponent *>(camera))
    {
        orbit_camera->SetupFromTransform();
    }

    pending_camera_.reset();
    Log(Info, "Session camera restored.");
}

void SessionManager::SaveSession(CameraComponent *camera)
{
    nlohmann::json session_json = nlohmann::json::object();
    session_json[SessionVersionKey] = SessionVersion;
    session_json[SessionConfigKey] = BuildConfigJson();

    if (camera)
    {
        session_json[SessionCameraKey] = BuildCameraJson(camera);
    }

    if (!WriteSessionJson(session_json))
    {
        Log(Warn, "Failed to save session.");
    }
}

void SessionManager::DrawUi(Scene *scene, bool need_default_sky, bool need_default_lighting)
{
    ASSERT(scene);

    ImGui::TextUnformatted("Session");
    ImGui::Separator();

    if (ImGui::Button("Load Last Session"))
    {
        if (LoadLastSession())
        {
            const auto scene_path = GetSceneConfigValue();
            SceneManager::LoadScene(scene, Path::Resource(scene_path), need_default_sky, need_default_lighting)
                ->Then([scene, this]() { ApplyCamera(scene->GetMainCamera()); });
        }
    }

    if (ImGui::Button("Save Session"))
    {
        SaveSession(scene->GetMainCamera());
    }
}
} // namespace sparkle
