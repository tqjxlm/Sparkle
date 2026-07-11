#include "io/scene/USDExporter.h"

#include "core/FileManager.h"
#include "core/Logger.h"
#include "io/Mesh.h"
#include "scene/Scene.h"
#include "scene/SceneNode.h"
#include "scene/component/camera/CameraComponent.h"
#include "scene/component/light/DirectionalLight.h"
#include "scene/component/light/SkyLight.h"
#include "scene/component/primitive/MeshPrimitive.h"
#include "scene/material/Material.h"

#include <cctype>
#include <core/model-scope.hh>
#include <format>
#include <optional>
#include <tinyusdz.hh>
#include <unordered_map>
#include <usda-writer.hh>

namespace sparkle
{
namespace
{
constexpr const char *MaterialRootPrimName = "_materials";
constexpr const char *TextureDirName = "textures";

// USD prim names must match [A-Za-z_][A-Za-z0-9_]*. the original name is kept as displayName metadata.
std::string MakeValidPrimName(const std::string &name)
{
    if (name.empty())
    {
        return "prim";
    }

    std::string valid_name = name;
    for (auto &c : valid_name)
    {
        if ((isalnum(static_cast<unsigned char>(c)) == 0) && c != '_')
        {
            c = '_';
        }
    }

    if (isdigit(static_cast<unsigned char>(valid_name[0])) != 0)
    {
        valid_name.insert(valid_name.begin(), '_');
    }

    return valid_name;
}

tinyusdz::value::matrix4d MatrixCast(const Mat4x4 &eigen_matrix)
{
    tinyusdz::value::matrix4d usd_matrix;
    for (auto i = 0; i < 4; i++)
    {
        for (auto j = 0; j < 4; j++)
        {
            // the inverse of USDLoader::MatrixCast
            usd_matrix.m[i][j] = static_cast<double>(eigen_matrix(j, i));
        }
    }

    return usd_matrix;
}

tinyusdz::value::color3f ColorCast(const Vector3 &v)
{
    return {v.x(), v.y(), v.z()};
}

struct ExportContext
{
    // parent directory of the output file. all relative asset references are resolved against it.
    Path output_dir;

    // material pointer -> authored prim name under the material root
    std::unordered_map<Material *, std::string> material_prim_names;

    // texture pointer -> file path relative to output_dir
    std::unordered_map<const Image2D *, std::string> exported_textures;

    tinyusdz::Prim material_root{tinyusdz::Scope()};

    unsigned num_meshes = 0;
    unsigned num_textures = 0;

    bool success = true;
};

// writes the texture image beside the output file and returns its path relative to output_dir.
std::string ExportTexture(ExportContext &ctx, const Image2D &image)
{
    if (auto it = ctx.exported_textures.find(&image); it != ctx.exported_textures.end())
    {
        return it->second;
    }

    const bool is_float = image.GetFormat() == PixelFormat::RGBAFloat || image.GetFormat() == PixelFormat::RGBAFloat16;

    // the extension must match what Image2D::WriteToFile encodes for this pixel format.
    // the index prefix avoids name collisions between textures that share a name.
    std::string file_name =
        std::format("{}_{}{}", ctx.num_textures, MakeValidPrimName(image.GetName()), is_float ? ".hdr" : ".png");
    ctx.num_textures++;

    std::string relative_path = std::string(TextureDirName) + "/" + file_name;

    Path file_path(ctx.output_dir.path / relative_path, ctx.output_dir.type);
    if (!image.WriteToFile(file_path))
    {
        Log(Error, "USDExporter: failed to write texture {}", file_path.path.string());
        ctx.success = false;
    }

    ctx.exported_textures.emplace(&image, relative_path);

    return relative_path;
}

// authors a UsdUVTexture shader prim that reads primvars:st through a UsdPrimvarReader_float2.
// returns the texture prim path for connection.
tinyusdz::Path AddTextureShader(ExportContext &ctx, tinyusdz::Prim &material_prim, const std::string &material_path,
                                const std::string &shader_name, const Image2D &image)
{
    const std::string texture_file = ExportTexture(ctx, image);

    tinyusdz::Shader shader;
    shader.name = shader_name;
    shader.info_id = tinyusdz::kUsdUVTexture;

    tinyusdz::UsdUVTexture texture;
    texture.file = tinyusdz::value::AssetPath(texture_file);
    texture.sourceColorSpace = IsSRGBFormat(image.GetFormat()) ? tinyusdz::UsdUVTexture::SourceColorSpace::SRGB
                                                               : tinyusdz::UsdUVTexture::SourceColorSpace::Raw;
    texture.wrapS = tinyusdz::UsdUVTexture::Wrap::Repeat;
    texture.wrapT = tinyusdz::UsdUVTexture::Wrap::Repeat;
    texture.st.set_connection(tinyusdz::Path(material_path + "/st_reader", "outputs:result"));
    texture.outputsRGB.set_authored(true);

    shader.value = std::move(texture);

    tinyusdz::Path texture_path(material_path + "/" + shader_name, "");

    std::string err;
    if (!material_prim.add_child(tinyusdz::Prim(shader), false, &err))
    {
        Log(Error, "USDExporter: failed to add texture shader {}: {}", shader_name, err);
        ctx.success = false;
    }

    return texture_path;
}

// authors a Material prim with a UsdPreviewSurface network equivalent to the given material.
// returns the material prim path for binding.
std::string ExportMaterial(ExportContext &ctx, Material *material)
{
    if (auto it = ctx.material_prim_names.find(material); it != ctx.material_prim_names.end())
    {
        return std::string("/") + MaterialRootPrimName + "/" + it->second;
    }

    const MaterialResource &resource = material->GetRawMaterial();

    std::string prim_name = MakeValidPrimName(resource.name);
    // materials may share a display name. suffix with an index to keep prim paths unique.
    prim_name = std::format("{}_{}", prim_name, ctx.material_prim_names.size());

    const std::string material_path = std::string("/") + MaterialRootPrimName + "/" + prim_name;

    tinyusdz::Material usd_material;
    usd_material.name = prim_name;
    usd_material.metas().set_displayName(resource.name);
    usd_material.surface.set(tinyusdz::Path(material_path + "/surface", "outputs:surface"));

    tinyusdz::Prim material_prim(usd_material);

    // uv reader shared by all textures of this material
    {
        tinyusdz::Shader reader_shader;
        reader_shader.name = "st_reader";
        reader_shader.info_id = tinyusdz::kUsdPrimvarReader_float2;

        tinyusdz::UsdPrimvarReader_float2 reader;
        reader.varname = std::string("st");
        reader.result.set_authored(true);

        reader_shader.value = std::move(reader);

        std::string err;
        if (!material_prim.add_child(tinyusdz::Prim(reader_shader), false, &err))
        {
            Log(Error, "USDExporter: failed to add primvar reader: {}", err);
            ctx.success = false;
        }
    }

    tinyusdz::UsdPreviewSurface surface;
    surface.outputsSurface.set_authored(true);
    surface.useSpecularWorkflow = 0;

    if (resource.base_color_texture)
    {
        auto texture_path =
            AddTextureShader(ctx, material_prim, material_path, "base_color_tex", *resource.base_color_texture);
        surface.diffuseColor.set_connection(tinyusdz::Path(texture_path.prim_part(), "outputs:rgb"));
    }
    else
    {
        surface.diffuseColor = ColorCast(resource.base_color);
    }

    if (resource.emissive_texture)
    {
        auto texture_path =
            AddTextureShader(ctx, material_prim, material_path, "emissive_tex", *resource.emissive_texture);
        surface.emissiveColor.set_connection(tinyusdz::Path(texture_path.prim_part(), "outputs:rgb"));
    }
    else
    {
        surface.emissiveColor = ColorCast(resource.emissive_color);
    }

    if (resource.normal_texture)
    {
        auto texture_path = AddTextureShader(ctx, material_prim, material_path, "normal_tex", *resource.normal_texture);
        surface.normal.set_connection(tinyusdz::Path(texture_path.prim_part(), "outputs:rgb"));
    }

    if (resource.metallic_roughness_texture)
    {
        // metallic and roughness are packed into one texture (gltf convention: G=roughness, B=metallic)
        auto texture_path = AddTextureShader(ctx, material_prim, material_path, "metallic_roughness_tex",
                                             *resource.metallic_roughness_texture);
        surface.metallic.set_connection(tinyusdz::Path(texture_path.prim_part(), "outputs:b"));
        surface.roughness.set_connection(tinyusdz::Path(texture_path.prim_part(), "outputs:g"));
    }
    else
    {
        surface.metallic = resource.metallic;
        surface.roughness = resource.roughness;
    }

    if (material->GetType() == Material::Dieletric)
    {
        // transmissive material. USDLoader maps opacity < 1 back to DieletricMaterial.
        surface.opacity = 0.f;
        surface.ior = resource.eta;
    }

    tinyusdz::Shader surface_shader;
    surface_shader.name = "surface";
    surface_shader.info_id = tinyusdz::kUsdPreviewSurface;
    surface_shader.value = std::move(surface);

    std::string err;
    if (!material_prim.add_child(tinyusdz::Prim(surface_shader), false, &err))
    {
        Log(Error, "USDExporter: failed to add surface shader: {}", err);
        ctx.success = false;
    }

    if (!ctx.material_root.add_child(std::move(material_prim), false, &err))
    {
        Log(Error, "USDExporter: failed to add material prim {}: {}", prim_name, err);
        ctx.success = false;
    }

    ctx.material_prim_names.emplace(material, prim_name);

    return material_path;
}

std::optional<tinyusdz::Prim> ExportMesh(ExportContext &ctx, const MeshPrimitive &primitive)
{
    const auto &mesh = primitive.GetMeshResource();
    if (!mesh || !mesh->Validate())
    {
        Log(Warn, "USDExporter: skipped mesh with invalid data");
        return std::nullopt;
    }

    if (!primitive.GetMaterial())
    {
        Log(Warn, "USDExporter: skipped mesh without material: {}", mesh->name);
        return std::nullopt;
    }

    tinyusdz::GeomMesh geom_mesh;
    geom_mesh.name = std::format("{}_{}", MakeValidPrimName(mesh->name.empty() ? "mesh" : mesh->name), ctx.num_meshes);
    ctx.num_meshes++;

    {
        std::vector<tinyusdz::value::point3f> points(mesh->vertices.size());
        for (size_t i = 0; i < mesh->vertices.size(); i++)
        {
            points[i] = {mesh->vertices[i].x(), mesh->vertices[i].y(), mesh->vertices[i].z()};
        }
        geom_mesh.points.set_value(std::move(points));
    }

    {
        std::vector<tinyusdz::value::normal3f> normals(mesh->normals.size());
        for (size_t i = 0; i < mesh->normals.size(); i++)
        {
            normals[i] = {mesh->normals[i].x(), mesh->normals[i].y(), mesh->normals[i].z()};
        }
        geom_mesh.normals.set_value(std::move(normals));
        geom_mesh.normals.metas().set_interpolation_enum(tinyusdz::Interpolation::Vertex);
    }

    {
        geom_mesh.faceVertexCounts.set_value(std::vector<int32_t>(mesh->GetNumFaces(), 3));

        std::vector<int32_t> indices(mesh->indices.size());
        for (size_t i = 0; i < mesh->indices.size(); i++)
        {
            indices[i] = static_cast<int32_t>(mesh->indices[i]);
        }
        geom_mesh.faceVertexIndices.set_value(std::move(indices));
    }

    std::string err;

    if (!mesh->uvs.empty())
    {
        tinyusdz::GeomPrimvar st;
        st.set_name("st");
        st.set_interpolation(tinyusdz::Interpolation::Vertex);

        std::vector<tinyusdz::value::texcoord2f> uvs(mesh->uvs.size());
        for (size_t i = 0; i < mesh->uvs.size(); i++)
        {
            uvs[i] = {mesh->uvs[i].x(), mesh->uvs[i].y()};
        }
        st.set_value(std::move(uvs));

        if (!geom_mesh.set_primvar(st, &err))
        {
            Log(Error, "USDExporter: failed to set st primvar: {}", err);
            ctx.success = false;
        }
    }

    if (!mesh->tangents.empty())
    {
        tinyusdz::GeomPrimvar tangents;
        tangents.set_name("tangents");
        tangents.set_interpolation(tinyusdz::Interpolation::Vertex);

        std::vector<tinyusdz::value::float4> tangent_values(mesh->tangents.size());
        for (size_t i = 0; i < mesh->tangents.size(); i++)
        {
            tangent_values[i] = {mesh->tangents[i].x(), mesh->tangents[i].y(), mesh->tangents[i].z(),
                                 mesh->tangents[i].w()};
        }
        tangents.set_value(std::move(tangent_values));

        if (!geom_mesh.set_primvar(tangents, &err))
        {
            Log(Error, "USDExporter: failed to set tangents primvar: {}", err);
            ctx.success = false;
        }
    }

    {
        const std::string material_path = ExportMaterial(ctx, primitive.GetMaterial());

        tinyusdz::Relationship binding;
        binding.set(tinyusdz::Path(material_path, ""));
        geom_mesh.materialBinding = binding;
    }

    return tinyusdz::Prim(geom_mesh);
}

tinyusdz::Prim ExportCamera(const CameraComponent &camera)
{
    const auto &attribute = camera.GetAttribute();

    tinyusdz::GeomCamera geom_camera;
    geom_camera.name = "camera";
    geom_camera.projection = tinyusdz::GeomCamera::Projection::Perspective;
    // USD lens attributes are in millimeters, ours are in meters
    geom_camera.focalLength = attribute.focal_length * 1000.f;
    geom_camera.verticalAperture = attribute.sensor_height * 1000.f;
    geom_camera.fStop = attribute.aperture;
    geom_camera.focusDistance = attribute.focus_distance;

    return tinyusdz::Prim(geom_camera);
}

tinyusdz::Prim ExportDirectionalLight(const DirectionalLight &light)
{
    tinyusdz::DistantLight distant_light;
    distant_light.name = "light";
    // our light color carries intensity. direction comes from the parent Xform.
    distant_light.color = ColorCast(light.GetColor());
    distant_light.intensity = 1.f;

    return tinyusdz::Prim(distant_light);
}

std::optional<tinyusdz::Prim> ExportSkyLight(ExportContext &ctx, const SkyLight &light)
{
    const auto &sky_map_path = light.GetSkyMapPath();
    if (sky_map_path.empty() || !light.GetSkyMap())
    {
        // procedural sky: color only
        tinyusdz::DomeLight dome_light;
        dome_light.name = "sky";
        dome_light.color = ColorCast(light.GetColor());
        dome_light.intensity = 1.f;

        return tinyusdz::Prim(dome_light);
    }

    // copy the sky map file beside the output so the exported scene is self-contained.
    // sky maps can be found either in resources or in generated files (see Image2D::LoadFromFile).
    auto *file_manager = FileManager::GetNativeFileManager();
    auto data = file_manager->Read(Path::Resource(sky_map_path));
    if (data.empty())
    {
        data = file_manager->Read(Path::Internal(sky_map_path));
    }

    if (data.empty())
    {
        Log(Error, "USDExporter: failed to read sky map {}", sky_map_path);
        ctx.success = false;
        return std::nullopt;
    }

    std::string relative_path =
        std::string(TextureDirName) + "/" + std::filesystem::path(sky_map_path).filename().string();

    Path target_path(ctx.output_dir.path / relative_path, ctx.output_dir.type);
    if (file_manager->Write(target_path, data).empty())
    {
        Log(Error, "USDExporter: failed to write sky map {}", target_path.path.string());
        ctx.success = false;
        return std::nullopt;
    }

    tinyusdz::DomeLight dome_light;
    dome_light.name = "sky";
    dome_light.file = tinyusdz::value::AssetPath(relative_path);
    dome_light.intensity = 1.f;

    return tinyusdz::Prim(dome_light);
}

tinyusdz::Prim ExportNode(ExportContext &ctx, SceneNode *node)
{
    tinyusdz::Xform xform;
    xform.name = MakeValidPrimName(std::string(node->GetName()));

    {
        tinyusdz::XformOp op;
        op.op_type = tinyusdz::XformOp::OpType::Transform;
        op.set_value(MatrixCast(node->GetLocalTransform().GetTransformData().matrix()));
        xform.xformOps.push_back(op);
    }

    tinyusdz::Prim prim(xform);
    prim.metas().set_displayName(std::string(node->GetName()));

    for (const auto &component : node->GetComponents())
    {
        std::optional<tinyusdz::Prim> component_prim;

        if (auto *mesh = dynamic_cast<MeshPrimitive *>(component.get()))
        {
            component_prim = ExportMesh(ctx, *mesh);
        }
        else if (auto *camera = dynamic_cast<CameraComponent *>(component.get()))
        {
            component_prim = ExportCamera(*camera);
        }
        else if (auto *directional_light = dynamic_cast<DirectionalLight *>(component.get()))
        {
            component_prim = ExportDirectionalLight(*directional_light);
        }
        else if (auto *sky_light = dynamic_cast<SkyLight *>(component.get()))
        {
            component_prim = ExportSkyLight(ctx, *sky_light);
        }
        else
        {
            Log(Warn, "USDExporter: skipped unsupported component on node {}", node->GetName());
        }

        if (component_prim)
        {
            std::string err;
            if (!prim.add_child(std::move(*component_prim), true, &err))
            {
                Log(Error, "USDExporter: failed to add component prim to {}: {}", node->GetName(), err);
                ctx.success = false;
            }
        }
    }

    for (const auto &child : node->GetChildren())
    {
        std::string err;
        if (!prim.add_child(ExportNode(ctx, child.get()), true, &err))
        {
            Log(Error, "USDExporter: failed to add child prim to {}: {}", node->GetName(), err);
            ctx.success = false;
        }
    }

    return prim;
}
} // namespace

bool USDExporter::Export(Scene *scene, const Path &output_file)
{
    ASSERT(scene);

    if (output_file.type == PathType::Resource || !output_file.IsValid())
    {
        Log(Error, "USDExporter: output path must be writable: {}", output_file.path.string());
        return false;
    }

    if (output_file.path.extension() != ".usda")
    {
        Log(Error, "USDExporter: only .usda output is supported: {}", output_file.path.string());
        return false;
    }

    ExportContext ctx;
    ctx.output_dir = Path(output_file.path.parent_path(), output_file.type);

    auto *file_manager = FileManager::GetNativeFileManager();
    if (!file_manager->TryCreateDirectory(Path(ctx.output_dir.path / TextureDirName, ctx.output_dir.type)))
    {
        Log(Error, "USDExporter: failed to create output directory {}", ctx.output_dir.path.string());
        return false;
    }

    {
        tinyusdz::Scope material_scope;
        material_scope.name = MaterialRootPrimName;
        ctx.material_root = tinyusdz::Prim(material_scope);
    }

    auto root_prim = ExportNode(ctx, scene->GetRootNode());
    const std::string root_prim_name = root_prim.element_name();

    tinyusdz::Stage stage;
    stage.metas().upAxis = tinyusdz::Axis::Z;
    stage.metas().metersPerUnit = 1.0;
    stage.metas().defaultPrim = tinyusdz::value::token(root_prim_name);

    if (!stage.add_root_prim(std::move(root_prim)))
    {
        Log(Error, "USDExporter: failed to add scene root prim: {}", stage.get_error());
        return false;
    }

    if (!ctx.material_root.children().empty())
    {
        if (!stage.add_root_prim(std::move(ctx.material_root)))
        {
            Log(Error, "USDExporter: failed to add material root prim: {}", stage.get_error());
            return false;
        }
    }

    if (!stage.commit())
    {
        Log(Error, "USDExporter: failed to commit stage: {}", stage.get_error());
        return false;
    }

    std::string warn;
    std::string err;
    if (!tinyusdz::usda::SaveAsUSDA(output_file.Resolved().string(), stage, &warn, &err))
    {
        Log(Error, "USDExporter: failed to save {}: {}", output_file.path.string(), err);
        return false;
    }

    if (!warn.empty())
    {
        Log(Warn, "USDExporter: saved {} with warning: {}", output_file.path.string(), warn);
    }

    if (!ctx.success)
    {
        Log(Error, "USDExporter: export finished with errors: {}", output_file.path.string());
        return false;
    }

    Log(Info, "USDExporter: exported scene to {}", output_file.Resolved().string());

    return true;
}
} // namespace sparkle
