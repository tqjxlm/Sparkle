#include "io/scene/GLTFLoader.h"

#include "core/FileManager.h"
#include "core/Logger.h"
#include "io/ImageTypes.h"
#include "io/Material.h"
#include "io/Mesh.h"
#include "scene/component/primitive/MeshPrimitive.h"
#include "scene/material/MaterialManager.h"
#include "scene/material/PbrMaterial.h"

// use our own stb headers instead of the bundled version from gltf
#define TINYGLTF_NO_INCLUDE_STB_IMAGE
#define TINYGLTF_NO_INCLUDE_STB_IMAGE_WRITE
#include <stb_image.h>
#include <stb_image_write.h>

#define TINYGLTF_USE_CPP14

#if FRAMEWORK_ANDROID
#define TINYGLTF_ANDROID_LOAD_FROM_ASSETS
#endif

#define TINYGLTF_IMPLEMENTATION
#include <tiny_gltf.h>

#include <algorithm>

namespace sparkle
{
/// Adapts an array of bytes to an array of T. Will advace of byte_stride each
/// elements.
template <typename T> struct ArrayAdapter
{
    /// Pointer to the bytes
    const unsigned char *dataPtr;
    /// Number of elements in the array
    const size_t elemCount;
    /// Stride in bytes between two elements
    const size_t stride;

    /// Construct an array adapter.
    /// \param ptr Pointer to the start of the data, with offset applied
    /// \param count Number of elements in the array
    /// \param byte_stride Stride betweens elements in the array
    ArrayAdapter(const unsigned char *ptr, size_t count, size_t byte_stride)
        : dataPtr(ptr), elemCount(count), stride(byte_stride)
    {
    }

    /// Returns a *copy* of a single element. Can't be used to modify it.
    const T &operator[](size_t pos) const
    {
        ASSERT_F(pos < elemCount,
                 "Tried to access beyond the last element of an array adapter with count {} while getting elemnet "
                 "number {}",
                 std::to_string(elemCount).c_str(), std::to_string(pos).c_str());

        return *(reinterpret_cast<const T *>(dataPtr + pos * stride));
    }
};

/// Interface of any adapted array that returns ingeger data
struct IntArrayBase
{
    virtual ~IntArrayBase() = default;
    virtual unsigned int operator[](size_t) const = 0;
    [[nodiscard]] virtual size_t Size() const = 0;
};

/// An array that loads interger types, returns them as int
template <class T> struct IntArray : public IntArrayBase
{
    ArrayAdapter<T> adapter;

    explicit IntArray(const ArrayAdapter<T> &a) : adapter(a)
    {
    }

    unsigned int operator[](size_t position) const override
    {
        return static_cast<unsigned int>(adapter[position]);
    }

    [[nodiscard]] size_t Size() const override
    {
        return adapter.elemCount;
    }
};

template <typename T> struct VectorArray
{
    ArrayAdapter<T> adapter;

    explicit VectorArray(const ArrayAdapter<T> &a) : adapter(a)
    {
    }

    const T &operator[](size_t position) const
    {
        return adapter[position];
    }

    [[nodiscard]] size_t Size() const
    {
        return adapter.elemCount;
    }
};

using V2fArray = VectorArray<Vector2>;
using V3fArray = VectorArray<Vector3>;
using V4fArray = VectorArray<Vector4>;
using V2dArray = VectorArray<Vector2d>;
using V3dArray = VectorArray<Vector3d>;
using V4dArray = VectorArray<Vector4d>;

template <typename SourceType, typename TargetType> static void ConvertVectorType(const SourceType &v, TargetType &vec)
{
    for (unsigned i = 0; i < std::min(static_cast<unsigned>(SourceType::SizeAtCompileTime),
                                      static_cast<unsigned>(TargetType::SizeAtCompileTime));
         i++)
    {
        vec[i] = static_cast<TargetType::Scalar>(v[i]);
    }
}

template <typename SourceType, typename TargetType>
static void LoadVaryingBufferToArray(std::vector<TargetType> &array, const unsigned char *data_ptr, size_t count,
                                     size_t byte_stride)
{
    const VectorArray<SourceType> buffer({data_ptr, count, byte_stride});
    array.resize(count);

    // For each triangle :
    for (size_t i = 0; i < count; ++i)
    {
        ConvertVectorType(buffer[i], array[i]);
    }
}

template <typename TargetType>
static void LoadVaryingBufferToArray(int type, int component_type, std::vector<TargetType> &array,
                                     const unsigned char *data_ptr, size_t count, size_t byte_stride)
{
    switch (type)
    {
    case TINYGLTF_TYPE_VEC4: {
        switch (component_type)
        {
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            LoadVaryingBufferToArray<Vector4>(array, data_ptr, count, byte_stride);
            break;
        case TINYGLTF_COMPONENT_TYPE_DOUBLE:
            LoadVaryingBufferToArray<Vector4d>(array, data_ptr, count, byte_stride);
            break;
        default:
            UnImplemented(component_type);
        }

        break;
    }
    case TINYGLTF_TYPE_VEC3: {
        switch (component_type)
        {
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            LoadVaryingBufferToArray<Vector3>(array, data_ptr, count, byte_stride);
            break;
        case TINYGLTF_COMPONENT_TYPE_DOUBLE:
            LoadVaryingBufferToArray<Vector3d>(array, data_ptr, count, byte_stride);
            break;
        default:
            UnImplemented(component_type);
        }

        break;
    }
    case TINYGLTF_TYPE_VEC2: {
        switch (component_type)
        {
        case TINYGLTF_COMPONENT_TYPE_FLOAT:
            LoadVaryingBufferToArray<Vector2>(array, data_ptr, count, byte_stride);
            break;
        case TINYGLTF_COMPONENT_TYPE_DOUBLE:
            LoadVaryingBufferToArray<Vector2d>(array, data_ptr, count, byte_stride);
            break;
        default:
            UnImplemented(component_type);
        }

        break;
    }
    default:
        UnImplemented(type);
    }
}

static void LoadPositionArray(Mesh &loaded_mesh, const Vector3 &scale, const tinygltf::Accessor &attrib_accessor,
                              const unsigned char *data_ptr, size_t count, size_t byte_stride)
{
    const Vector3 p_min = utilities::Vector2Vec3(attrib_accessor.minValues).cwiseProduct(scale);
    const Vector3 p_max = utilities::Vector2Vec3(attrib_accessor.maxValues).cwiseProduct(scale);

    loaded_mesh.center = (p_max + p_min) * 0.5f;
    loaded_mesh.extent = (p_max - p_min) * 0.5f;

    switch (attrib_accessor.type)
    {
    case TINYGLTF_TYPE_VEC3: {
        switch (attrib_accessor.componentType)
        {
        case TINYGLTF_COMPONENT_TYPE_FLOAT: {
            const V3fArray positions(ArrayAdapter<Vector3>(data_ptr, count, byte_stride));
            loaded_mesh.vertices.resize(positions.Size());

            for (size_t i{0}; i < positions.Size(); ++i)
            {
                const Vector3 v = positions[i].cwiseProduct(scale);

                loaded_mesh.vertices[i] = v;
            }
        }
        break;
        default:
            ASSERT(false);
        }
        break;
    case TINYGLTF_COMPONENT_TYPE_DOUBLE: {
        switch (attrib_accessor.type)
        {
        case TINYGLTF_TYPE_VEC3: {
            const V3dArray positions(ArrayAdapter<Vector3d>(data_ptr, count, byte_stride));
            loaded_mesh.vertices.resize(positions.Size());

            for (size_t i{0}; i < positions.Size(); ++i)
            {
                const Vector3 v = positions[i].cast<Scalar>().cwiseProduct(scale);

                loaded_mesh.vertices[i] = v;
            }
        }
        break;
        default:
            UnImplemented(attrib_accessor.type);
            break;
        }
        break;
    default:
        break;
    }
    }
    }
}

static std::shared_ptr<Image2D> CreateImage2D(const tinygltf::Image &image, bool is_linear)
{
    if (image.component != 4)
    {
        // gltf should have made sure it to be 4
        UnImplemented(image.component);
        return nullptr;
    }

    PixelFormat format = PixelFormat::Count;
    switch (image.pixel_type)
    {
    case TINYGLTF_COMPONENT_TYPE_BYTE:
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        format = is_linear ? PixelFormat::R8G8B8A8_UNORM : PixelFormat::R8G8B8A8_SRGB;
        break;
    default:
        UnImplemented(image.pixel_type);
        return nullptr;
    }

    return std::make_shared<Image2D>(image.width, image.height, format, image.image);
}

static std::shared_ptr<Image2D> CreateTexture(const tinygltf::Model &model, uint32_t texture_index, bool is_linear)
{
    if (texture_index == UINT_MAX)
    {
        return nullptr;
    }
    auto image_index = static_cast<unsigned>(model.textures[texture_index].source);
    auto image = model.images[image_index];
    return CreateImage2D(image, is_linear);
}

static std::shared_ptr<Mesh> LoadPrimitive(const tinygltf::Model &model, const tinygltf::Primitive &primitive,
                                           const Vector3 &scale)
{
    auto loaded_mesh_ptr = std::make_shared<Mesh>();

    Mesh &loaded_mesh = *loaded_mesh_ptr;

    bool converted_to_triangle_list = false;
    std::unique_ptr<IntArrayBase> indices_array_ptr = nullptr;
    {
        const auto &indices_accessor = model.accessors[static_cast<unsigned>(primitive.indices)];
        const auto &buffer_view = model.bufferViews[static_cast<unsigned>(indices_accessor.bufferView)];
        const auto &buffer = model.buffers[static_cast<unsigned>(buffer_view.buffer)];
        const auto *const data_ptr = buffer.data.data() + buffer_view.byteOffset + indices_accessor.byteOffset;
        const auto byte_stride = static_cast<unsigned>(indices_accessor.ByteStride(buffer_view));
        const auto count = indices_accessor.count;

        switch (indices_accessor.componentType)
        {
        case TINYGLTF_COMPONENT_TYPE_BYTE:
            indices_array_ptr = std::make_unique<IntArray<int8_t>>(ArrayAdapter<int8_t>(data_ptr, count, byte_stride));
            break;

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
            indices_array_ptr =
                std::make_unique<IntArray<uint8_t>>(ArrayAdapter<uint8_t>(data_ptr, count, byte_stride));
            break;

        case TINYGLTF_COMPONENT_TYPE_SHORT:
            indices_array_ptr =
                std::make_unique<IntArray<int16_t>>(ArrayAdapter<int16_t>(data_ptr, count, byte_stride));
            break;

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
            indices_array_ptr =
                std::make_unique<IntArray<uint16_t>>(ArrayAdapter<uint16_t>(data_ptr, count, byte_stride));
            break;

        case TINYGLTF_COMPONENT_TYPE_INT:
            indices_array_ptr =
                std::make_unique<IntArray<int32_t>>(ArrayAdapter<int32_t>(data_ptr, count, byte_stride));
            break;

        case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
            indices_array_ptr =
                std::make_unique<IntArray<uint32_t>>(ArrayAdapter<uint32_t>(data_ptr, count, byte_stride));
            break;
        default:
            break;
        }
    }
    const auto &indices = *indices_array_ptr;

    if (indices_array_ptr)
    {
        loaded_mesh.indices.resize(indices_array_ptr->Size());
        for (size_t i(0); i < indices_array_ptr->Size(); ++i)
        {
            loaded_mesh.indices[i] = indices[i];
        }
    }

    switch (primitive.mode)
    {
    case TINYGLTF_MODE_TRIANGLE_FAN:
        if (!converted_to_triangle_list)
        {
            converted_to_triangle_list = true;

            auto triangle_fan = std::move(loaded_mesh.indices);
            loaded_mesh.indices.resize((triangle_fan.size() - 2) * 3);

            for (size_t i{2}; i < triangle_fan.size(); ++i)
            {
                loaded_mesh.indices[i * 3 + 0] = triangle_fan[0];
                loaded_mesh.indices[i * 3 + 1] = triangle_fan[i - 1];
                loaded_mesh.indices[i * 3 + 2] = triangle_fan[i];
            }
        }
        FMT_FALLTHROUGH;
    case TINYGLTF_MODE_TRIANGLE_STRIP:
        if (!converted_to_triangle_list)
        {
            converted_to_triangle_list = true;

            auto triangle_strip = std::move(loaded_mesh.indices);
            loaded_mesh.indices.resize((triangle_strip.size() - 2) * 3);

            for (size_t i{2}; i < triangle_strip.size(); ++i)
            {
                loaded_mesh.indices[i * 3 + 0] = triangle_strip[i - 2];
                loaded_mesh.indices[i * 3 + 1] = triangle_strip[i - 1];
                loaded_mesh.indices[i * 3 + 2] = triangle_strip[i];
            }
        }
        FMT_FALLTHROUGH;
    case TINYGLTF_MODE_TRIANGLES: {
        for (const auto &attribute : primitive.attributes)
        {
            const auto attrib_accessor = model.accessors[static_cast<unsigned>(attribute.second)];
            const auto &buffer_view = model.bufferViews[static_cast<unsigned>(attrib_accessor.bufferView)];
            const auto &buffer = model.buffers[static_cast<unsigned>(buffer_view.buffer)];
            const auto *const data_ptr = buffer.data.data() + buffer_view.byteOffset + attrib_accessor.byteOffset;
            const auto byte_stride = static_cast<unsigned>(attrib_accessor.ByteStride(buffer_view));
            const auto count = attrib_accessor.count;

            if (attribute.first == "POSITION")
            {
                LoadPositionArray(loaded_mesh, scale, attrib_accessor, data_ptr, count, byte_stride);
            }
            else if (attribute.first == "NORMAL")
            {
                ASSERT_EQUAL(attrib_accessor.type, TINYGLTF_TYPE_VEC3);
                LoadVaryingBufferToArray(attrib_accessor.type, attrib_accessor.componentType, loaded_mesh.normals,
                                         data_ptr, count, byte_stride);
            }
            else if (attribute.first == "TANGENT")
            {
                ASSERT_EQUAL(attrib_accessor.type, TINYGLTF_TYPE_VEC4);
                LoadVaryingBufferToArray(attrib_accessor.type, attrib_accessor.componentType, loaded_mesh.tangents,
                                         data_ptr, count, byte_stride);
            }
            else if (attribute.first == "TEXCOORD_0")
            {
                ASSERT_EQUAL(attrib_accessor.type, TINYGLTF_TYPE_VEC2);
                LoadVaryingBufferToArray(attrib_accessor.type, attrib_accessor.componentType, loaded_mesh.uvs, data_ptr,
                                         count, byte_stride);
            }
        }

        break;
    }

    case TINYGLTF_MODE_POINTS:
    case TINYGLTF_MODE_LINE:
    case TINYGLTF_MODE_LINE_LOOP:
        Log(Error, "primitive is not triangle based, ignoring");
        break;
    default:
        UnImplemented(primitive.mode);
        break;
    }

    return loaded_mesh_ptr;
}

static std::vector<std::shared_ptr<Material>> LoadMaterials(const tinygltf::Model &model)
{
    auto &material_manager = MaterialManager::Instance();

    std::vector<std::shared_ptr<Material>> material_resources;
    for (const auto &material : model.materials)
    {
        MaterialResource raw_material;

        if (material.pbrMetallicRoughness.baseColorTexture.index >= 0)
        {
            auto base_color_texture = CreateTexture(
                model, static_cast<unsigned>(material.pbrMetallicRoughness.baseColorTexture.index), false);
            raw_material.base_color_texture = base_color_texture;
            raw_material.base_color = utilities::Vector2Vec3(material.pbrMetallicRoughness.baseColorFactor);
        }

        if (material.normalTexture.index >= 0)
        {
            auto normal_map = CreateTexture(model, static_cast<unsigned>(material.normalTexture.index), true);
            raw_material.normal_texture = normal_map;
        }

        if (material.pbrMetallicRoughness.metallicRoughnessTexture.index >= 0)
        {
            auto metallic_roughness_texture = CreateTexture(
                model, static_cast<unsigned>(material.pbrMetallicRoughness.metallicRoughnessTexture.index), true);
            raw_material.metallic_roughness_texture = metallic_roughness_texture;
            raw_material.metallic = static_cast<float>(material.pbrMetallicRoughness.metallicFactor);
            raw_material.roughness = static_cast<float>(material.pbrMetallicRoughness.roughnessFactor);
        }

        if (material.emissiveTexture.index >= 0)
        {
            auto emissive_texture = CreateTexture(model, static_cast<unsigned>(material.emissiveTexture.index), false);
            raw_material.emissive_texture = emissive_texture;
            raw_material.emissive_color = utilities::Vector2Vec3(material.emissiveFactor);
        }

        raw_material.name = material.name;

        auto material_resource = material_manager.GetOrCreateMaterial<PbrMaterial>(raw_material);
        material_resources.emplace_back(material_resource);
    }

    return material_resources;
}

static std::shared_ptr<SceneNode> ProcessNode(const tinygltf::Model &model, unsigned node_id,
                                              const std::vector<std::shared_ptr<Material>> &materials, Scene *scene)
{
    const auto &node = model.nodes[node_id];

    std::shared_ptr<SceneNode> scene_node = std::make_shared<SceneNode>(scene, node.name);
    Rotation rotation = node.rotation.empty() ? Rotation::Identity()
                                              : utilities::Vector4AsQuaternion(utilities::Vector2Vec4(node.rotation));

    // gltf assumes y-up coordinates. we need z-up here
    rotation = Eigen::AngleAxis<Scalar>(utilities::ToRadian(90.0f), Right) * rotation;

    auto translation = node.translation.empty() ? Zeros : utilities::Vector2Vec3(node.translation);
    auto scale = node.scale.empty() ? Ones : utilities::Vector2Vec3(node.scale);

    // scale is applied immediately on loading, so leave it as ones here
    scene_node->SetTransform(translation, rotation.normalized(), Ones);

    if (node.mesh > -1)
    {
        const auto &mesh = model.meshes[static_cast<size_t>(node.mesh)];

        // every primitive corresponds to a mesh component
        for (auto primitive_id = 0u; primitive_id < mesh.primitives.size(); ++primitive_id)
        {
            const auto &primitive = mesh.primitives[primitive_id];
            auto mesh_resource = LoadPrimitive(model, primitive, scale);

            mesh_resource->name = std::format("{}_{}", mesh.name, primitive_id);

            auto mesh_component = std::make_shared<MeshPrimitive>(mesh_resource);
            mesh_component->SetMaterial(materials[static_cast<size_t>(primitive.material)]);

            scene_node->AddComponent(mesh_component);
        }
    }

    if (!node.children.empty())
    {
        // has children, keep traversing
        for (auto i : node.children)
        {
            scene_node->AddChild(ProcessNode(model, static_cast<unsigned>(i), materials, scene));
        }
    }

    return scene_node;
}

std::shared_ptr<SceneNode> GLTFLoader::Load(const std::string &path, Scene *scene)
{
    Log(Debug, "GLTFLoader: begin loading model {}", path);

    tinygltf::Model model;
    std::string err;
    std::string warn;

    const auto &data = FileManager::GetNativeFileManager()->ReadResource(path);
    if (data.empty())
    {
        Log(Error, "failed to find model {}", path);
        return nullptr;
    }

    std::filesystem::path fs_path(path);
    auto parent_path = fs_path.parent_path().string();

    ASSERT(data.size() < std::numeric_limits<int>::max());
    const bool ret = loader_->LoadASCIIFromString(&model, &err, &warn, data.data(),
                                                  static_cast<unsigned int>(data.size()), parent_path);

    if (!ret)
    {
        Log(Error, "failed to load model from {}. parent path {}", path, parent_path);
        Log(Error, "{}", err);
        return nullptr;
    }

    if (!warn.empty())
    {
        Log(Warn, "loading model with warning: {}", path);
        Log(Warn, "{}", warn);
    }

    auto loaded_root = std::make_shared<SceneNode>(scene, path);

    std::vector<int> *nodes_to_traverse_ref;
    std::vector<int> nodes_to_traverse;

    if (model.scenes.empty())
    {
        // this is unusual, try to parse it as a flat scene with a dummy scene root
        nodes_to_traverse.resize(model.nodes.size());
        for (unsigned i = 0; i < model.nodes.size(); i++)
        {
            nodes_to_traverse[i] = static_cast<int>(i);
        }
        nodes_to_traverse_ref = &nodes_to_traverse;
    }
    else
    {
        auto scene_to_display = model.defaultScene > -1 ? static_cast<unsigned>(model.defaultScene) : 0;
        nodes_to_traverse_ref = &model.scenes[scene_to_display].nodes;
    }

    auto materials = LoadMaterials(model);

    for (auto i : *nodes_to_traverse_ref)
    {
        loaded_root->AddChild(ProcessNode(model, static_cast<unsigned>(i), materials, scene));
    }

    return loaded_root;
}

GLTFLoader::GLTFLoader()
{
    loader_ = std::make_shared<tinygltf::TinyGLTF>();

    tinygltf::FsCallbacks file_callback;
    file_callback.ExpandFilePath = [](const std::string &filepath, void *) { return filepath; };
    file_callback.ReadWholeFile = [](std::vector<unsigned char> *out, std::string *, const std::string &filepath,
                                     void *) {
        auto *file_manager = FileManager::GetNativeFileManager();
        auto data = file_manager->ReadResource(filepath);
        if (data.empty())
        {
            return false;
        }
        out->resize(data.size());
        std::ranges::copy(data, out->begin());
        return true;
    };
    file_callback.WriteWholeFile = [](std::string *, const std::string &filepath,
                                      const std::vector<unsigned char> &contents, void *) {
        auto *file_manager = FileManager::GetNativeFileManager();

        auto write_out_path =
            file_manager->WriteFile(filepath, reinterpret_cast<const char *>(contents.data()), contents.size(), false);
        return !write_out_path.empty();
    };
    file_callback.FileExists = [](const std::string &filepath, void *) {
        auto *file_manager = FileManager::GetNativeFileManager();

        return file_manager->ResourceExists(filepath);
    };
    file_callback.GetFileSizeInBytes = [](size_t *filesize_out, std::string *err, const std::string &abs_filename,
                                          void *) {
        auto *file_manager = FileManager::GetNativeFileManager();
        size_t size = file_manager->GetResourceSize(abs_filename);
        if (size == std::numeric_limits<size_t>::max())
        {
            *err = std::format("file not exists! {}", abs_filename);
            return false;
        }

        *filesize_out = size;
        return true;
    };
    file_callback.user_data = nullptr;
    loader_->SetFsCallbacks(file_callback);
}

GLTFLoader::~GLTFLoader() = default;

} // namespace sparkle
