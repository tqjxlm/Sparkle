#pragma once

#include "rhi/RHIResource.h"

#include "core/Exception.h"
#include "core/Logger.h"

namespace sparkle
{
enum class RHIShaderStage : uint8_t
{
    Vertex = 0,
    Pixel,
    Compute,
    Count
};

struct RHIShaderStageUsage
{
    uint8_t Vertex : 1 = 0;
    uint8_t Pixel : 1 = 0;
    uint8_t Compute : 1 = 0;

    [[nodiscard]] bool UsedInStage(RHIShaderStage stage) const
    {
        switch (stage)
        {
        case RHIShaderStage::Vertex:
            return Vertex;
        case RHIShaderStage::Pixel:
            return Pixel;
        case RHIShaderStage::Compute:
            return Compute;
        case RHIShaderStage::Count:
            UnImplemented(stage);
        }
        return false;
    }
};

// type and name information for resource(s) declared in shaders
// - any mismatched property between c++ and shader will cause a fatal error
struct RHIShaderResourceReflection
{
    enum class ResourceType : uint8_t
    {
        UniformBuffer,
        DynamicUniformBuffer,
        StorageBuffer,
        Texture2D,
        Sampler,
        StorageImage2D,
        AccelerationStructure,
    };

    std::string_view name;
    uint32_t set = UINT_MAX;
    uint32_t slot = UINT_MAX;
    ResourceType type;
    bool is_bindless = false;

    RHIShaderResourceReflection(std::string_view in_name, ResourceType in_type, bool in_is_bindless)
        : name(in_name), type(in_type), is_bindless(in_is_bindless)
    {
    }

    [[nodiscard]] uint32_t GetHash() const;
};

class RHIShaderResourceTable;
class RHIShaderResourceSet;

// binding point between resource(s) declared in shaders and resource(s) used at run time
// - it is generated with RHIShaderResourceReflection by RHI-specific reflection procedure
class RHIShaderResourceBinding
{
public:
    static constexpr uint32_t MaxBindlessResources = 1024;

    RHIShaderResourceBinding(RHIShaderResourceTable *resource_table, std::string_view name,
                             RHIShaderResourceReflection::ResourceType type, bool is_bindless);

    void UpdateReflectionIndex(uint32_t set, uint32_t slot)
    {
        decl_->set = set;
        decl_->slot = slot;
    }

    [[nodiscard]] RHIResource *GetResource() const
    {
        return resource_;
    }

    [[nodiscard]] const RHIShaderResourceReflection *GetReflection() const
    {
        return decl_.get();
    }

    [[nodiscard]] bool IsRegistered() const
    {
        return decl_ != nullptr;
    }

    [[nodiscard]] bool IsBindless() const
    {
        return decl_->is_bindless;
    }

    [[nodiscard]] RHIShaderResourceReflection::ResourceType GetType() const
    {
        return decl_->type;
    }

    void Print()
    {
        Log(Warn, "Resource {}, array? {}", decl_->name, decl_->is_bindless);
    }

    void SetParentSet(RHIShaderResourceSet *set)
    {
        parent_set_ = set;
    }

protected:
    void BindResource(RHIResource *resource, bool rebind);

    void Register(std::unique_ptr<RHIShaderResourceReflection> &&decl)
    {
        ASSERT(!decl_);
        decl_ = std::move(decl);
    }

private:
    std::unique_ptr<RHIShaderResourceReflection> decl_;
    RHIResource *resource_ = nullptr;
    RHIShaderResourceSet *parent_set_ = nullptr;
};

class RHIBuffer;
class RHIImageView;
class RHISampler;
class RHITLAS;
class RHIResourceArray;

template <RHIShaderResourceReflection::ResourceType Type, bool IsArray>
class RHIShaderResourceBindingTyped : public RHIShaderResourceBinding
{
public:
    RHIShaderResourceBindingTyped(RHIShaderResourceTable *resource_table, std::string_view name)
        : RHIShaderResourceBinding(resource_table, name, Type, IsArray)
    {
    }

    // this layer of wrapping is for contraining the type of resource that can be bound
    template <class T>
        requires std::derived_from<T, RHIResource>
    void BindResource(T *resource, bool rebind = false)
    {
        if constexpr (IsArray)
        {
            static_assert(std::derived_from<T, RHIResourceArray>);

            // TODO(tqjxlm): check the underlying type statically
            ASSERT(resource->GetUnderlyingType() == Type);
        }
        else
        {
            if constexpr (Type == RHIShaderResourceReflection::ResourceType::UniformBuffer)
            {
                static_assert(std::derived_from<T, RHIBuffer>);
            }
            else if constexpr (Type == RHIShaderResourceReflection::ResourceType::DynamicUniformBuffer)
            {
                static_assert(std::derived_from<T, RHIBuffer>);
            }
            else if constexpr (Type == RHIShaderResourceReflection::ResourceType::StorageBuffer)
            {
                static_assert(std::derived_from<T, RHIBuffer>);
            }
            else if constexpr (Type == RHIShaderResourceReflection::ResourceType::Texture2D)
            {
                static_assert(std::derived_from<T, RHIImageView>, "Did you forget to call GetView() on the texture?");
            }
            else if constexpr (Type == RHIShaderResourceReflection::ResourceType::Sampler)
            {
                static_assert(std::derived_from<T, RHISampler>);
            }
            else if constexpr (Type == RHIShaderResourceReflection::ResourceType::StorageImage2D)
            {
                static_assert(std::derived_from<T, RHIImageView>);
            }
            else if constexpr (Type == RHIShaderResourceReflection::ResourceType::AccelerationStructure)
            {
                static_assert(std::derived_from<T, RHITLAS>);
            }
            else
            {
                UnImplemented(Type);
            }
        }

        // check ok, hand over to parent class
        RHIShaderResourceBinding::BindResource(resource, rebind);
    }

    // this layer of wrapping is for type matching
    template <class T>
        requires std::derived_from<T, RHIResource>
    void BindResource(RHIResourceRef<T> resource, bool rebind = false)
    {
        BindResource(resource.get(), rebind);
    }
};

// a resource set is a group of shader resources that share the same set id
// it is an abstraction for VkDescriptorSet
class RHIShaderResourceSet
{
public:
    void MarkResourceDirty()
    {
        resource_dirty_ = true;
    }

    void SetBinding(unsigned slot, RHIShaderResourceBinding *binding)
    {
        if (!binding)
        {
            return;
        }

        if (bindings_.size() < slot + 1)
        {
            bindings_.resize(slot + 1);
        }

        bindings_[slot] = binding;
        binding->SetParentSet(this);
    }

    void MergeWith(const RHIShaderResourceSet &other)
    {
        bindings_.resize(std::max(bindings_.size(), other.bindings_.size()));

        for (auto binding_id = 0u; binding_id < other.bindings_.size(); binding_id++)
        {
            SetBinding(binding_id, other.bindings_[binding_id]);
        }
    }

    [[nodiscard]] uint32_t GetResourceHash() const
    {
        if (resource_dirty_)
        {
            UpdateResourceHash();
        }

        return resource_hash_;
    }

    [[nodiscard]] uint32_t GetLayoutHash() const
    {
        ASSERT_F(layout_hash_ != 0, "should call UpdateLayoutHash before this");
        return layout_hash_;
    }

    [[nodiscard]] const auto &GetBindings() const
    {
        return bindings_;
    }

    void UpdateLayoutHash();

private:
    void UpdateResourceHash() const;

    mutable bool resource_dirty_ = true;

    mutable uint32_t resource_hash_;

    uint32_t layout_hash_ = 0;

    std::vector<RHIShaderResourceBinding *> bindings_;
};

// a resource table contains info of all resources used by a shader, including:
// 1. the logical layout identified by (set id, binding id)
// 2. the actual bound resource at each slot
class RHIShaderResourceTable
{
public:
    virtual ~RHIShaderResourceTable() = default;

    [[nodiscard]] const std::vector<RHIShaderResourceSet> &GetResourceSets() const
    {
        return resource_sets_;
    }

    [[nodiscard]] const auto &GetBindingMap() const
    {
        return binding_map_;
    }

    [[nodiscard]] const auto &GetBindings() const
    {
        return bindings_;
    }

    void Initialize();

    void RegisterShaderResourceReflection(RHIShaderResourceBinding *binding, RHIShaderResourceReflection *decl);

protected:
    bool initialized_ = false;

    std::vector<RHIShaderResourceBinding *> bindings_;
    std::vector<RHIShaderResourceSet> resource_sets_;
    std::unordered_map<std::string_view, RHIShaderResourceBinding *> binding_map_;
};

class RHIShaderInfo
{
public:
    virtual ~RHIShaderInfo() = default;

    [[nodiscard]] virtual std::unique_ptr<RHIShaderResourceTable> CreateShaderResourceTable() const = 0;

    [[nodiscard]] const std::string &GetEntryPoint() const
    {
        return entry_point_;
    }

    [[nodiscard]] const std::string &GetName() const
    {
        return name_;
    }

    [[nodiscard]] RHIShaderStage GetStage() const
    {
        return stage_;
    }

    [[nodiscard]] const std::string &GetPath() const
    {
        return path_;
    }

protected:
    RHIShaderInfo() = default;

    RHIShaderStage stage_;
    std::string name_;
    std::string path_;
    std::string entry_point_;
};

#define REGISTGER_SHADER(class_name, stage, path, entry_point)                                                         \
private:                                                                                                               \
    class_name()                                                                                                       \
    {                                                                                                                  \
        stage_ = stage;                                                                                                \
        path_ = path;                                                                                                  \
        entry_point_ = entry_point;                                                                                    \
        name_ = #class_name;                                                                                           \
    }                                                                                                                  \
                                                                                                                       \
public:                                                                                                                \
    static RHIShaderStage GetStage()                                                                                   \
    {                                                                                                                  \
        return stage;                                                                                                  \
    }                                                                                                                  \
                                                                                                                       \
    static const class_name *GetShaderInfo()                                                                           \
    {                                                                                                                  \
        static const class_name instance;                                                                              \
        return &instance;                                                                                              \
    }

// NOLINTBEGIN(bugprone-macro-parentheses)
#define BEGIN_SHADER_RESOURCE_TABLE(BaseClass)                                                                         \
public:                                                                                                                \
    class ResourceTable : public BaseClass                                                                             \
    {
// NOLINTEND(bugprone-macro-parentheses)

#define END_SHADER_RESOURCE_TABLE                                                                                      \
    }                                                                                                                  \
    ;                                                                                                                  \
                                                                                                                       \
    std::unique_ptr<RHIShaderResourceTable> CreateShaderResourceTable() const override                                 \
    {                                                                                                                  \
        return std::make_unique<ResourceTable>();                                                                      \
    }

// NOLINTBEGIN(readability-identifier-naming)
#define USE_SHADER_RESOURCE_INTERNAL(name, type, is_bindless)                                                          \
public:                                                                                                                \
    RHIShaderResourceBindingTyped<type, is_bindless> &name()                                                           \
    {                                                                                                                  \
        return ShaderResource_##name;                                                                                  \
    }                                                                                                                  \
                                                                                                                       \
protected:                                                                                                             \
    RHIShaderResourceBindingTyped<type, is_bindless> ShaderResource_##name = {this, #name};

#define USE_SHADER_RESOURCE(name, type) USE_SHADER_RESOURCE_INTERNAL(name, type, false)
#define USE_SHADER_RESOURCE_BINDLESS(name, type) USE_SHADER_RESOURCE_INTERNAL(name, type, true)

// NOLINTEND(readability-identifier-naming)

class RHIShader : public RHIResource
{
public:
    explicit RHIShader(const RHIShaderInfo *shader_info)
        : RHIResource(shader_info->GetName()), shader_info_(shader_info)
    {
    }

    [[nodiscard]] bool IsValid() const
    {
        return loaded_;
    }

    [[nodiscard]] const RHIShaderInfo *GetInfo() const
    {
        return shader_info_;
    }

    virtual void Load() = 0;

protected:
    const RHIShaderInfo *shader_info_;
    bool loaded_ = false;
};
} // namespace sparkle
