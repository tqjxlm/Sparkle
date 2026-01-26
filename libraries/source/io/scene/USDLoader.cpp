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
#include "scene/component/primitive/MeshPrimitive.h"
#include "scene/material/MaterialManager.h"
#include "scene/material/PbrMaterial.h"

#include <filesystem>
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

    // assume tightly packed and no multi-sample
    ASSERT_EQUAL(attribute.elementSize, 1);
    ASSERT_EQUAL(attribute.stride, 0);

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
    const tinyusdz::Stage &stage;
    const tinyusdz::tydra::RenderScene &render_scene;
};

static std::shared_ptr<CameraComponent> LoadCamera(const tinyusdz::tydra::Node &node, const USDLoaderContext &ctx)
{
    const auto &prim = ctx.stage.GetPrimAtPath(tinyusdz::Path(node.abs_path, ""));
    const auto *render_camera = prim.value()->as<tinyusdz::GeomCamera>();

    tinyusdz::GeomCamera::Projection projection;
    render_camera->projection.get_value().get(0, &projection);
    if (projection != tinyusdz::GeomCamera::Projection::Perspective)
    {
        ASSERT_F(false, "USDLoader: only perspective camera is supported for now. node: {}.", node.display_name);
        return nullptr;
    }

    float focal_length = 35.f;
    render_camera->focalLength.get_value().get(0, &focal_length);

    float aperture = 22.f;
    render_camera->verticalAperture.get_value().get(0, &aperture);

    auto camera = std::make_shared<OrbitCameraComponent>(CameraComponent::Attribute{
        .focal_length = focal_length * 0.001f, // convert mm to m
        .aperture = aperture,
    });

    // TODO(tqjxlm): this means we can only have one camera. should support multiple cameras in the future.
    ctx.scene->SetMainCamera(camera);

    return camera;
}

static std::shared_ptr<Image2D> CreateTexture(const USDLoaderContext &ctx, size_t texture_id)
{
    auto image_id = static_cast<size_t>(ctx.render_scene.textures[texture_id].texture_image_id);
    const auto &image = ctx.render_scene.images[image_id];
    const auto &image_data = ctx.render_scene.buffers[static_cast<size_t>(image.buffer_id)];

    // TODO(tqjxlm): support 3 channels and 1 channel textures
    ASSERT_EQUAL(image.channels, 4);

    PixelFormat format = PixelFormat::Count;
    switch (image.assetTexelComponentType)
    {
    case tinyusdz::tydra::ComponentType::UInt8:
    case tinyusdz::tydra::ComponentType::Int8: {
        // TODO(tqjxlm): sRGB vs Lin_sRGB?
        bool is_srgb = image.colorSpace == tinyusdz::tydra::ColorSpace::sRGB ||
                       image.usdColorSpace == tinyusdz::tydra::ColorSpace::Lin_sRGB;

        format = is_srgb ? PixelFormat::R8G8B8A8_SRGB : PixelFormat::R8G8B8A8_UNORM;
        break;
    }
    case tinyusdz::tydra::ComponentType::Half:
        format = PixelFormat::RGBAFloat16;
        break;
    case tinyusdz::tydra::ComponentType::Float:
        format = PixelFormat::RGBAFloat;
        break;
    default:
        UnImplemented(format);
        return nullptr;
    }

    return std::make_shared<Image2D>(image.width, image.height, format, image_data.data);
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

    if (render_material.surfaceShader.diffuseColor.is_texture())
    {
        material_resource.base_color_texture =
            CreateTexture(ctx, static_cast<size_t>(render_material.surfaceShader.diffuseColor.texture_id));
    }
    else
    {
        material_resource.base_color = MatrixCast(render_material.surfaceShader.diffuseColor.value);
    }

    if (render_material.surfaceShader.emissiveColor.is_texture())
    {
        material_resource.emissive_texture =
            CreateTexture(ctx, static_cast<size_t>(render_material.surfaceShader.emissiveColor.texture_id));
    }
    else
    {
        material_resource.emissive_color = MatrixCast(render_material.surfaceShader.emissiveColor.value);
    }

    if (render_material.surfaceShader.normal.is_texture())
    {
        material_resource.normal_texture =
            CreateTexture(ctx, static_cast<size_t>(render_material.surfaceShader.normal.texture_id));
    }

    if (render_material.surfaceShader.metallic.is_texture())
    {
        // Curretly we assume metallic and roughness are packed into a single texture.
        ASSERT_EQUAL(render_material.surfaceShader.metallic.texture_id,
                     render_material.surfaceShader.roughness.texture_id);

        material_resource.metallic_roughness_texture =
            CreateTexture(ctx, static_cast<size_t>(render_material.surfaceShader.metallic.texture_id));
    }
    else
    {
        material_resource.metallic = render_material.surfaceShader.metallic.value;
        material_resource.roughness = render_material.surfaceShader.roughness.value;
    }

    // TODO(tqjxlm): support dieletric materials
    material_resource.eta = 0;
    material_resource.name = render_material.name;

    auto material = material_manager.GetOrCreateMaterial<PbrMaterial>(material_resource);

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

static std::shared_ptr<SceneNode> LoadNode(const tinyusdz::tydra::Node &node, const USDLoaderContext &ctx)
{
    auto scene_node =
        std::make_shared<SceneNode>(ctx.scene, node.display_name.empty() ? node.prim_name : node.display_name);

    scene_node->SetTransform(MatrixCast(node.local_matrix));

    for (const auto &child : node.children)
    {
        switch (child.nodeType)
        {
        case tinyusdz::tydra::NodeType::Xform:
            if (auto child_node = LoadNode(child, ctx))
            {
                scene_node->AddChild(child_node);
            }
            break;
        case tinyusdz::tydra::NodeType::Mesh:
            scene_node->AddComponent(LoadMesh(child, ctx));
            break;
        case tinyusdz::tydra::NodeType::Camera:
            scene_node->AddComponent(LoadCamera(child, ctx));
            break;
        case tinyusdz::tydra::NodeType::DirectionalLight:
            scene_node->AddComponent(LoadDirectionalLight(child, ctx));
            break;
        case tinyusdz::tydra::NodeType::Skeleton:
        case tinyusdz::tydra::NodeType::PointLight:
        case tinyusdz::tydra::NodeType::EnvmapLight:
            Log(Warn, "USDLoader: Skipped unsupported node type {}", Enum2Str(node.nodeType));
            return nullptr;
        default:
            UnImplemented(child.nodeType);
            return nullptr;
        }
    }

    return scene_node;
}

static int TinyusdzAssetReadFun(const char *resolved_asset_name, uint64_t req_nbytes, uint8_t *out_buf,
                                uint64_t *nbytes, std::string *err, void *userdata)
{
    auto *file_manager = reinterpret_cast<FileManager *>(userdata);

    auto data = file_manager->Read(Path::Resource(resolved_asset_name));
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
    auto *file_manager = reinterpret_cast<FileManager *>(userdata);

    // assume single search path for now
    *resolved_asset_name = search_paths[0] + "/" + asset_name;

    if (file_manager->Exists(Path::Resource(*resolved_asset_name)))
    {
        return 0;
    }

    *err = std::format("TinyUSDZ: failed to find asset {} under {}", asset_name, search_paths[0]);

    return 1;
}

static std::shared_ptr<SceneNode> LoadScene(const std::string &path, const tinyusdz::Stage &stage, Scene *scene)
{
    std::filesystem::path file_path(path);

    tinyusdz::tydra::RenderSceneConverter converter;

    tinyusdz::tydra::RenderSceneConverterEnv env(stage);

    // for consistency
    env.mesh_config.build_vertex_indices = true;

    // for performance, we respect the original texel bitdepth.
    env.material_config.preserve_texel_bitdepth = true;

    // redirect all asset handling to our file manager.
    tinyusdz::AssetResolutionHandler asset_handler;
    asset_handler.userdata = FileManager::GetNativeFileManager();
    asset_handler.read_fun = &TinyusdzAssetReadFun;
    asset_handler.resolve_fun = &TinyusdzAssetResolveFun;

    env.asset_resolver.register_asset_resolution_handler("", asset_handler);

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
        .stage = stage,
        .render_scene = render_scene,
    };

    auto root_node = LoadNode(render_scene.nodes[render_scene.default_root_node], ctx);

    // TODO(tqjxlm): support multiple cameras
    auto *main_camera = static_cast<OrbitCameraComponent *>(scene->GetMainCamera());

    AABB total_bound;
    std::vector<SceneNode *> nodes_to_visit;
    nodes_to_visit.push_back(root_node.get());
    while (!nodes_to_visit.empty())
    {
        std::vector<SceneNode *> next_nodes;
        for (auto *node : nodes_to_visit)
        {
            for (const auto &component : node->GetComponents())
            {
                if (total_bound.IsValid())
                {
                    total_bound += component->GetWorldBoundingBox();
                }
                else
                {
                    total_bound = component->GetWorldBoundingBox();
                }
            }

            for (const auto &child : node->GetChildren())
            {
                next_nodes.push_back(child.get());
            }
        }

        std::swap(nodes_to_visit, next_nodes);
    }

    main_camera->SetCenter(total_bound.Center());

    return root_node;
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

    auto root_node = LoadScene(path_string, stage, scene);
    if (root_node)
    {
        root_node->SetName(asset_root_.path.string());
    }

    return root_node;
}
} // namespace sparkle
