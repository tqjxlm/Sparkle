#include "scene/material/MaterialManager.h"

#include "core/Logger.h"
#include "scene/material/LambertianMaterial.h"
#include "scene/material/Material.h"
#include "scene/material/MetalMaterial.h"

namespace sparkle
{
MaterialManager *MaterialManager::instance_ = nullptr;

std::unique_ptr<MaterialManager> MaterialManager::CreateInstance()
{
    ASSERT(!instance_);
    auto instance = std::make_unique<MaterialManager>();
    instance_ = instance.get();

    return instance;
}

MaterialManager::MaterialManager()
{
    metals_.resize(MetalType::COUNT);

    default_material_ = GetOrCreateMaterial<LambertianMaterial>({.base_color = Ones, .name = "DefaultMaterial"});
    metals_[GOLD] = GetOrCreateMaterial<MetalMaterial>({.base_color = {1.0f, 0.7f, 0.29f}, .name = "Gold"});
    metals_[IRON] = GetOrCreateMaterial<MetalMaterial>({.base_color = {0.56f, 0.57f, 0.58f}, .name = "Iron"});
    metals_[BRONZE] = GetOrCreateMaterial<MetalMaterial>({.base_color = {0.95f, 0.64f, 0.54f}, .name = "Bronze"});
    metals_[ALUMINIUM] = GetOrCreateMaterial<MetalMaterial>({.base_color = {0.92f, 0.92f, 0.92f}, .name = "Aluminium"});
    metals_[SILVER] = GetOrCreateMaterial<MetalMaterial>({.base_color = {0.95f, 0.93f, 0.88f}, .name = "Silver"});
}

MaterialManager::~MaterialManager() = default;

void MaterialManager::Destroy()
{
    valid_ = false;

    metals_.clear();
    default_material_ = nullptr;

    Log(Debug, "MaterialManager destroyed");
}

std::shared_ptr<Material> MaterialManager::GetRandomMetalMaterial()
{
    auto material_id = static_cast<unsigned>(sampler::RandomUnit<true>() * static_cast<unsigned>(MetalType::COUNT - 1));
    return metals_[material_id];
}
} // namespace sparkle
