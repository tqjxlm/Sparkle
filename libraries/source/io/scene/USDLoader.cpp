#include "io/scene/USDLoader.h"

#include "core/Enum.h"
#include "core/Exception.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "io/Mesh.h"
#include "scene/Scene.h"
#include "scene/SceneNode.h"
#include "scene/component/camera/OrbitCameraComponent.h"
#include "scene/component/light/DirectionalLight.h"
#include "scene/component/light/SkyLight.h"
#include "scene/component/primitive/MeshPrimitive.h"
#include "scene/material/DieletricMaterial.h"
#include "scene/material/MaterialManager.h"
#include "scene/material/PbrMaterial.h"

#include <filesystem>
#include <limits>
#include <tinyusdz.hh>
#include <tydra/render-data.hh>

namespace sparkle
{
static Mat4x4 MatrixCast(const tinyusdz::value::matrix4d &v)
{
    Mat4x4 eigen_matrix;
    for (auto i = 0; i < 4; i++)
    {
        for (auto j = 0; j < 4; j++)
        {
            // TODO(tqjxlm): support double matrix
            eigen_matrix(j, i) = static_cast<Scalar>(v.m[i][j]);
        }
    }

    return eigen_matrix;
}

static Vector3 MatrixCast(const tinyusdz::value::color3f &v)
{
    return {v.r, v.g, v.b};
}

static Vector3 MatrixCast(const std::array<Scalar, 3> &v)
{
    return {v[0], v[1], v[2]};
}

template <typename T>
static void CopyAttributeBuffer(const tinyusdz::tydra::VertexAttribute &attribute, std::vector<T> &dst)
{
    ASSERT_EQUAL(attribute.format_size(), sizeof(T));

    // assume tightly packed and no multi-sample. tydra expresses tightly packed
    // either as stride 0 or as an explicit format_size stride, depending on the code path.
    ASSERT_EQUAL(attribute.elementSize, 1);
    ASSERT(attribute.stride == 0 || attribute.stride == attribute.format_size());

    dst.resize(attribute.data.size() / attribute.format_size());
    ASSERT_EQUAL(dst.size() * sizeof(T), attribute.data.size());

    memcpy(reinterpret_cast<uint8_t *>(dst.data()), attribute.get_data().data(), attribute.get_data().size());
}

template <typename T, typename U> static void CopyVectorBuffer(const std::vector<T> &src, std::vector<U> &dst)
{
    ASSERT_EQUAL(sizeof(T), sizeof(U));

    dst.resize(src.size());
    memcpy(reinterpret_cast<uint8_t *>(dst.data()), src.data(), src.size() * sizeof(src[0]));
}

struct USDLoaderContext
{
    Scene *scene;
    const Path &asset_root;
    const tinyusdz::Stage &stage;
    const tinyusdz::tydra::RenderScene &render_scene;
};

static std::shared_ptr<CameraComponent> LoadCamera(const tinyusdz::tydra::Node &node, const USDLoaderContext &ctx)
{
    const auto &prim = ctx.stage.GetPrimAtPath(tinyusdz::Path(node.abs_path, ""));
    const auto *render_camera = prim.value()->as<tinyusdz::GeomCamera>();

    auto projection = tinyusdz::GeomCamera::Projection::Perspective;
    render_camera->projection.get_value().get_scalar(&projection);
    if (projection != tinyusdz::GeomCamera::Projection::Perspective)
    {
        ASSERT_F(false, "USDLoader: only perspective camera is supported for now. node: {}.", node.display_name);
        return nullptr;
    }

    CameraComponent::Attribute attribute;

    // USD lens attributes are in millimeters, ours are in meters
    float focal_length = 35.f;
    render_camera->focalLength.get_value().get(0, &focal_length);
    attribute.focal_length = focal_length * 0.001f;

    float vertical_aperture = attribute.sensor_height * 1000.f;
    render_camera->verticalAperture.get_value().get(0, &vertical_aperture);
    attribute.sensor_height = vertical_aperture * 0.001f;

    // 0 means not authored for both attributes. fall back to our defaults.
    float f_stop = 0.f;
    render_camera->fStop.get_value().get(0, &f_stop);
    if (f_stop > 0.f)
    {
        attribute.aperture = f_stop;
    }

    float focus_distance = 0.f;
    render_camera->focusDistance.get_value().get(0, &focus_distance);
    if (focus_distance > 0.f)
    {
        attribute.focus_distance = focus_distance;
    }

    auto camera = std::make_shared<OrbitCameraComponent>(attribute);

    // TODO(tqjxlm): this means we can only have one camera. should support multiple cameras in the future.
    ctx.scene->SetMainCamera(camera);

    return camera;
}

static std::shared_ptr<Image2D> CreateTexture(const USDLoaderContext &ctx, size_t texture_id)
{
    if (texture_id >= ctx.render_scene.textures.size())
    {
        Log(Error, "USDLoader: texture id is out of range: {}", texture_id);
        return nullptr;
    }

    // tydra keeps the texture entry but leaves these ids at -1 when it fails to load the image
    auto image_id = ctx.render_scene.textures[texture_id].texture_image_id;
    if (image_id < 0 || static_cast<size_t>(image_id) >= ctx.render_scene.images.size())
    {
        Log(Error, "USDLoader: texture has no image. texture id: {}", texture_id);
        return nullptr;
    }

    const auto &image = ctx.render_scene.images[static_cast<size_t>(image_id)];
    if (!image.decoded || image.width <= 0 || image.height <= 0 || image.channels != 4)
    {
        Log(Error, "USDLoader: invalid decoded texture image {}: {}x{}, {} channels", image.asset_identifier,
            image.width, image.height, image.channels);
        return nullptr;
    }

    if (image.buffer_id < 0 || static_cast<size_t>(image.buffer_id) >= ctx.render_scene.buffers.size())
    {
        Log(Error, "USDLoader: texture image has no data: {}", image.asset_identifier);
        return nullptr;
    }

    const auto &image_data = ctx.render_scene.buffers[static_cast<size_t>(image.buffer_id)];

    PixelFormat format = PixelFormat::Count;
    switch (image.assetTexelComponentType)
    {
    case tinyusdz::tydra::ComponentType::UInt8:
    case tinyusdz::tydra::ComponentType::Int8: {
        switch (image.colorSpace)
        {
        case tinyusdz::tydra::ColorSpace::sRGB:
        case tinyusdz::tydra::ColorSpace::sRGB_Texture:
            format = PixelFormat::R8G8B8A8Srgb;
            break;
        case tinyusdz::tydra::ColorSpace::Lin_sRGB:
        case tinyusdz::tydra::ColorSpace::Lin_Rec709:
        case tinyusdz::tydra::ColorSpace::Raw:
            format = PixelFormat::R8G8B8A8Unorm;
            break;
        default:
            Log(Error, "USDLoader: unsupported 8-bit texture color space {} for {}",
                tinyusdz::tydra::to_string(image.colorSpace), image.asset_identifier);
            return nullptr;
        }
        break;
    }
    case tinyusdz::tydra::ComponentType::Half:
        format = PixelFormat::RGBAFloat16;
        break;
    case tinyusdz::tydra::ComponentType::Float:
        format = PixelFormat::RGBAFloat;
        break;
    default:
        Log(Error, "USDLoader: unsupported texture component type {} for {}",
            tinyusdz::tydra::to_string(image.assetTexelComponentType), image.asset_identifier);
        return nullptr;
    }

    const auto width = static_cast<size_t>(image.width);
    const auto height = static_cast<size_t>(image.height);
    const auto pixel_size = static_cast<size_t>(GetPixelSize(format));
    if (width > std::numeric_limits<size_t>::max() / height ||
        width * height > std::numeric_limits<size_t>::max() / pixel_size)
    {
        Log(Error, "USDLoader: texture dimensions overflow storage size: {}", image.asset_identifier);
        return nullptr;
    }

    const auto expected_size = width * height * pixel_size;
    if (image_data.data.size() != expected_size)
    {
        Log(Error, "USDLoader: texture data size mismatch for {}: expected {}, got {}", image.asset_identifier,
            expected_size, image_data.data.size());
        return nullptr;
    }

    return std::make_shared<Image2D>(static_cast<unsigned>(width), static_cast<unsigned>(height), format,
                                     image_data.data);
}

static std::shared_ptr<MeshPrimitive> LoadMesh(const tinyusdz::tydra::Node &node, const USDLoaderContext &ctx)
{
    ASSERT(node.id >= 0);

    auto id = static_cast<size_t>(node.id);
    const auto &render_mesh = ctx.render_scene.meshes[id];

    if (!render_mesh.is_single_indexable)
    {
        Log(Warn, "USDLoader: Mesh {} is not single indexable. Skipped.", node.display_name);
        return nullptr;
    }

    if (render_mesh.material_id < 0)
    {
        Log(Warn, "USDLoader: Mesh {} does not have material. Skipped.", node.display_name);
        return nullptr;
    }

    const auto &render_material = ctx.render_scene.materials[static_cast<size_t>(render_mesh.material_id)];

    auto mesh = std::make_shared<Mesh>();
    mesh->name = node.prim_name;

    mesh->indices = render_mesh.faceVertexIndices();

    CopyVectorBuffer(render_mesh.points, mesh->vertices);

    Vector3 p_min = mesh->vertices[0];
    Vector3 p_max = mesh->vertices[0];
    for (const auto &v : mesh->vertices)
    {
        p_min = p_min.cwiseMin(v);
        p_max = p_max.cwiseMax(v);
    }

    mesh->center = (p_max + p_min) * 0.5f;
    mesh->extent = (p_max - p_min) * 0.5f;

    ASSERT(!render_mesh.normals.empty());
    CopyAttributeBuffer(render_mesh.normals, mesh->normals);

    if (render_mesh.texcoords.empty())
    {
        // TODO(tqjxlm): allow empty uv attribute to save some bandwidth
        mesh->uvs.resize(mesh->vertices.size());

        // we will need tangent in the end, so generate in advance
        mesh->tangents.reserve(mesh->vertices.size());
        for (auto i = 0u; i < mesh->vertices.size(); i++)
        {
            const Vector3 major_axis = utilities::GetPossibleMajorAxis(mesh->normals[i]);
            mesh->tangents.emplace_back(
                utilities::ConcatVector(mesh->normals[i].cross(major_axis).cross(mesh->normals[i]), 1.0f));
        }
    }
    else
    {
        // TODO(tqjxlm): support multi-texcoords
        CopyAttributeBuffer(render_mesh.texcoords.begin()->second, mesh->uvs);

        // if there are texcoords, we assume the mesh has tangents
        ASSERT(!render_mesh.tangents.empty());

        if (render_mesh.tangents.format == tinyusdz::tydra::VertexAttributeFormat::Vec3)
        {
            auto vector_size = render_mesh.tangents.format_size();
            ASSERT(vector_size == sizeof(tinyusdz::tydra::vec3));
            ASSERT(render_mesh.tangents.get_data().size() == render_mesh.tangents.vertex_count() * vector_size);

            for (size_t i = 0; i < render_mesh.tangents.vertex_count(); i++)
            {
                auto element =
                    *reinterpret_cast<const tinyusdz::tydra::vec3 *>(&render_mesh.tangents.get_data()[i * vector_size]);
                mesh->tangents.emplace_back(element[0], element[1], element[2], 1.0f);
            }
        }
        else
        {
            ASSERT_EQUAL(render_mesh.tangents.format, tinyusdz::tydra::VertexAttributeFormat::Vec4);
            CopyAttributeBuffer(render_mesh.tangents, mesh->tangents);
        }
    }

    ASSERT(mesh->Validate());

    auto mesh_primitive = std::make_shared<MeshPrimitive>(mesh);

    auto &material_manager = MaterialManager::Instance();

    MaterialResource material_resource;

    bool is_dieletric = false;

    if (render_material.hasUsdPreviewSurface())
    {
        const auto &surface_shader = *render_material.surfaceShader;

        // a transmissive UsdPreviewSurface maps to our dieletric material (see USDExporter)
        is_dieletric = !surface_shader.opacity.is_texture() && surface_shader.opacity.value < 1.f;

        if (surface_shader.diffuseColor.is_texture())
        {
            material_resource.base_color_texture =
                CreateTexture(ctx, static_cast<size_t>(surface_shader.diffuseColor.texture_id));
        }
        else
        {
            material_resource.base_color = MatrixCast(surface_shader.diffuseColor.value);
        }

        if (surface_shader.emissiveColor.is_texture())
        {
            material_resource.emissive_texture =
                CreateTexture(ctx, static_cast<size_t>(surface_shader.emissiveColor.texture_id));

            // the texture is multiplied by emissive_color, whose default of Zeros would erase it
            material_resource.emissive_color = Ones;
        }
        else
        {
            material_resource.emissive_color = MatrixCast(surface_shader.emissiveColor.value);
        }

        if (surface_shader.normal.is_texture())
        {
            material_resource.normal_texture =
                CreateTexture(ctx, static_cast<size_t>(surface_shader.normal.texture_id));
        }

        if (surface_shader.metallic.is_texture())
        {
            // Curretly we assume metallic and roughness are packed into a single texture.
            // the b/g channel connections may get distinct tydra texture entries, but must share one image.
            ASSERT(surface_shader.roughness.is_texture());

            const auto &textures = ctx.render_scene.textures;
            ASSERT_EQUAL(textures[static_cast<size_t>(surface_shader.metallic.texture_id)].texture_image_id,
                         textures[static_cast<size_t>(surface_shader.roughness.texture_id)].texture_image_id);

            material_resource.metallic_roughness_texture =
                CreateTexture(ctx, static_cast<size_t>(surface_shader.metallic.texture_id));
        }
        else
        {
            material_resource.metallic = surface_shader.metallic.value;
            material_resource.roughness = surface_shader.roughness.value;
        }
    }

    material_resource.name = render_material.name;

    std::shared_ptr<Material> material;
    if (is_dieletric)
    {
        material_resource.eta = render_material.surfaceShader->ior.value;
        material = material_manager.GetOrCreateMaterial<DieletricMaterial>(material_resource);
    }
    else
    {
        material_resource.eta = 0;
        material = material_manager.GetOrCreateMaterial<PbrMaterial>(material_resource);
    }

    mesh_primitive->SetMaterial(material);

    return mesh_primitive;
}

static std::shared_ptr<DirectionalLight> LoadDirectionalLight(const tinyusdz::tydra::Node &node,
                                                              const USDLoaderContext &ctx)
{
    auto light = std::make_shared<DirectionalLight>();

    // tydra does not convert lights for now. we have to retrieve info from the stage.
    const auto &prim = ctx.stage.GetPrimAtPath(tinyusdz::Path(node.abs_path, ""));
    const auto *light_prim = prim.value()->as<tinyusdz::DistantLight>();

    // TODO(tqjxlm): support animated attributes
    tinyusdz::value::color3f color;
    light_prim->color.get_value().get(0, &color);

    float intensity = 1.0f;
    light_prim->intensity.get_value().get(0, &intensity);

    // in our framework, intensity is part of color.
    light->SetColor(MatrixCast(color) * intensity);

    return light;
}

static std::shared_ptr<SkyLight> LoadSkyLight(const tinyusdz::tydra::Node &node, const USDLoaderContext &ctx)
{
    // tydra does not convert dome lights for now. we have to retrieve info from the stage.
    const auto &prim = ctx.stage.GetPrimAtPath(tinyusdz::Path(node.abs_path, ""));
    const auto *light_prim = prim ? prim.value()->as<tinyusdz::DomeLight>() : nullptr;
    if (!light_prim)
    {
        Log(Warn, "USDLoader: Skipped EnvmapLight that is not a DomeLight. node: {}", node.abs_path);
        return nullptr;
    }

    tinyusdz::value::AssetPath asset_path;
    if (auto file_attribute = light_prim->file.get_value())
    {
        file_attribute.value().get(0, &asset_path);
    }

    auto light = std::make_shared<SkyLight>();

    if (asset_path.GetAssetPath().empty())
    {
        // procedural sky: color only
        tinyusdz::value::color3f color;
        light_prim->color.get_value().get(0, &color);

        float intensity = 1.0f;
        light_prim->intensity.get_value().get(0, &intensity);

        light->SetColor(MatrixCast(color) * intensity);
        return light;
    }

    // the texture path is relative to the USD file
    auto sky_map_path = (ctx.asset_root.path.parent_path() / asset_path.GetAssetPath()).lexically_normal().string();

    light->SetSkyMap(sky_map_path);

    return light;
}

static std::shared_ptr<SceneNode> LoadNode(const tinyusdz::tydra::Node &node, const USDLoaderContext &ctx)
{
    auto scene_node =
        std::make_shared<SceneNode>(ctx.scene, node.display_name.empty() ? node.prim_name : node.display_name);

    scene_node->SetTransform(MatrixCast(node.local_matrix));

    for (const auto &child : node.children)
    {
        std::shared_ptr<Component> component;

        switch (child.nodeType)
        {
        case tinyusdz::tydra::NodeType::Xform:
            if (auto child_node = LoadNode(child, ctx))
            {
                scene_node->AddChild(child_node);
            }
            continue;
        case tinyusdz::tydra::NodeType::Mesh:
            component = LoadMesh(child, ctx);
            break;
        case tinyusdz::tydra::NodeType::Camera:
            component = LoadCamera(child, ctx);
            break;
        case tinyusdz::tydra::NodeType::DirectionalLight:
            component = LoadDirectionalLight(child, ctx);
            break;
        case tinyusdz::tydra::NodeType::EnvmapLight:
            component = LoadSkyLight(child, ctx);
            break;
        default:
            Log(Warn, "USDLoader: Skipped unsupported node type {}. node: {}", Enum2Str(child.nodeType),
                child.display_name);
            continue;
        }

        if (component)
        {
            scene_node->AddComponent(component);
        }
    }

    return scene_node;
}

// asset paths inside a USD file live in the same storage domain (resource / internal / ...) as the file itself
struct TinyusdzAssetUserData
{
    FileManager *file_manager;
    PathType path_type;
};

static int TinyusdzAssetSizeFun(const char *resolved_asset_name, uint64_t *nbytes, std::string *err, void *userdata)
{
    const auto *user_data = reinterpret_cast<TinyusdzAssetUserData *>(userdata);

    auto size = user_data->file_manager->GetSize(Path(resolved_asset_name, user_data->path_type));
    if (size == 0)
    {
        *err = std::format("TinyUSDZ: failed to get asset size. asset {}", resolved_asset_name);
        return -1;
    }

    *nbytes = size;

    return 0;
}

static int TinyusdzAssetReadFun(const char *resolved_asset_name, uint64_t req_nbytes, uint8_t *out_buf,
                                uint64_t *nbytes, std::string *err, void *userdata)
{
    const auto *user_data = reinterpret_cast<TinyusdzAssetUserData *>(userdata);

    auto data = user_data->file_manager->Read(Path(resolved_asset_name, user_data->path_type));
    if (data.size() < req_nbytes)
    {
        *err = std::format("TinyUSDZ: asset size smaller than requested. loaded {}. requested {}. asset {}",
                           data.size(), req_nbytes, resolved_asset_name);
        return -1;
    }

    memcpy(out_buf, data.data(), req_nbytes);

    *nbytes = data.size();
    *err = "";

    return 0;
}

static int TinyusdzAssetResolveFun(const char *asset_name, const std::vector<std::string> &search_paths,
                                   std::string *resolved_asset_name, std::string *err, void *userdata)
{
    const auto *user_data = reinterpret_cast<TinyusdzAssetUserData *>(userdata);

    // assume single search path for now. path-join instead of string concat: the search path is
    // empty for a USD file at a storage root, and archive-backed storage (e.g. Android assets)
    // needs ".." normalized out for literal lookups.
    *resolved_asset_name = (std::filesystem::path(search_paths[0]) / asset_name).lexically_normal().string();

    if (user_data->file_manager->Exists(Path(*resolved_asset_name, user_data->path_type)))
    {
        return 0;
    }

    *err = std::format("TinyUSDZ: failed to find asset {} under {}", asset_name, search_paths[0]);

    return 1;
}

static std::shared_ptr<SceneNode> LoadScene(const Path &asset_root, const tinyusdz::Stage &stage, Scene *scene)
{
    const std::filesystem::path &file_path = asset_root.path;

    tinyusdz::tydra::RenderSceneConverter converter;

    tinyusdz::tydra::RenderSceneConverterEnv env(stage);

    // for consistency
    env.mesh_config.build_vertex_indices = true;

    // for performance, we respect the original texel bitdepth.
    env.material_config.preserve_texel_bitdepth = true;

    // redirect all asset handling to our file manager.
    TinyusdzAssetUserData asset_user_data{FileManager::GetNativeFileManager(), asset_root.type};

    tinyusdz::AssetResolutionHandler asset_handler;
    asset_handler.userdata = &asset_user_data;
    asset_handler.size_fun = &TinyusdzAssetSizeFun;
    asset_handler.read_fun = &TinyusdzAssetReadFun;
    asset_handler.resolve_fun = &TinyusdzAssetResolveFun;

    // tinyusdz dispatches handlers by file extension, so one registration does not cover all assets
    for (const auto *extension : {"", "png", "jpg", "jpeg", "hdr", "exr"})
    {
        env.asset_resolver.register_asset_resolution_handler(extension, asset_handler);
    }

    // we assume all dependencies are under the same directory as the main file.
    env.asset_resolver.set_search_paths({file_path.parent_path().string()});

    tinyusdz::tydra::RenderScene render_scene;
    bool convert_ok = converter.ConvertToRenderScene(env, &render_scene);
    if (!converter.GetWarning().empty())
    {
        Log(Warn, "USDLoader: ConvertToRenderScene WARN: {}", converter.GetWarning());
    }

    if (!convert_ok)
    {
        Log(Error, "USDLoader: Failed to convert USD Stage to OpenGL-like data structure: {}", converter.GetError());
        return nullptr;
    }

    USDLoaderContext ctx{
        .scene = scene,
        .asset_root = asset_root,
        .stage = stage,
        .render_scene = render_scene,
    };

    return LoadNode(render_scene.nodes[render_scene.default_root_node], ctx);
}

std::shared_ptr<SceneNode> USDLoader::Load(Scene *scene)
{
    tinyusdz::Stage stage;
    std::string warn;
    std::string err;

    auto path_string = asset_root_.path.string();

    auto data = FileManager::GetNativeFileManager()->Read(asset_root_);

    if (data.empty())
    {
        Log(Error, "USDLoader: failed to load file {}. {}", path_string, err);
        return nullptr;
    }

    // Auto detect USDA/USDC/USDZ
    bool ret = tinyusdz::LoadUSDFromMemory(reinterpret_cast<uint8_t *>(data.data()), data.size(), path_string, &stage,
                                           &warn, &err);

    if (!warn.empty())
    {
        Log(Warn, "USDLoader: loading file {} with warning {}", path_string, warn);
    }

    if (!ret)
    {
        if (!err.empty())
        {
            Log(Error, "USDLoader: loading file {} failed. {}", path_string, err);
        }
        return nullptr;
    }

    Log(Debug, "USDLoader: loading file {} ok", path_string);

    auto root_node = LoadScene(asset_root_, stage, scene);
    if (root_node)
    {
        root_node->SetName(asset_root_.path.string());
    }

    return root_node;
}
} // namespace sparkle
