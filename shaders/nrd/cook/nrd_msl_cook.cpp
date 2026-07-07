// Build-host tool: cross-compiles one NRD SPIR-V shader to MSL and prints the reflection metadata
// the runtime would otherwise need spirv-cross for (see shaders/nrd/cook/cook_nrd_shaders.py, which
// drives it, and docs/Nrd.md). Compiled at build time with the Vulkan SDK's spirv-cross static libs;
// never part of the app.
//
// usage: nrd_msl_cook <in.spv> <out.metal> <macos|ios>
// stdout: entry <name>
//         wg <x> <y> <z>
//         cb <msl index or -1>
//         srv|uav|sampler <count> {<spirv binding> <msl index>}...

#include <spirv_cross/spirv_msl.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace
{
void PrintIndexPairs(const char *tag, const spirv_cross::CompilerMSL &compiler,
                     const spirv_cross::SmallVector<spirv_cross::Resource> &resources)
{
    printf("%s %zu", tag, resources.size());
    for (const auto &resource : resources)
    {
        printf(" %u %u", compiler.get_decoration(resource.id, spv::DecorationBinding),
               compiler.get_automatic_msl_resource_binding(resource.id));
    }
    printf("\n");
}
} // namespace

int main(int argc, char **argv)
{
    if (argc != 4)
    {
        fprintf(stderr, "usage: %s <in.spv> <out.metal> <macos|ios>\n", argv[0]);
        return 1;
    }

    std::ifstream input(argv[1], std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (bytes.empty() || bytes.size() % 4 != 0)
    {
        fprintf(stderr, "invalid spirv: %s\n", argv[1]);
        return 1;
    }

    spirv_cross::CompilerMSL compiler(reinterpret_cast<const uint32_t *>(bytes.data()), bytes.size() / 4);
    spirv_cross::CompilerMSL::Options options;
    options.set_msl_version(3, 0, 0);
    options.platform = std::string(argv[3]) == "ios" ? spirv_cross::CompilerMSL::Options::iOS
                                                     : spirv_cross::CompilerMSL::Options::macOS;
    compiler.set_msl_options(options);

    const std::string msl = compiler.compile();
    if (msl.empty())
    {
        fprintf(stderr, "cross-compilation produced no output: %s\n", argv[1]);
        return 1;
    }
    std::ofstream(argv[2]) << msl;

    const auto &entry_points = compiler.get_entry_points_and_stages();
    const std::string entry =
        compiler.get_cleansed_entry_point_name(entry_points[0].name, entry_points[0].execution_model);
    const auto &entry_point = compiler.get_entry_point(entry_points[0].name, entry_points[0].execution_model);
    printf("entry %s\n", entry.c_str());
    printf("wg %u %u %u\n", entry_point.workgroup_size.x, entry_point.workgroup_size.y, entry_point.workgroup_size.z);

    const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
    if (resources.uniform_buffers.empty())
    {
        printf("cb -1\n");
    }
    else
    {
        printf("cb %u\n", compiler.get_automatic_msl_resource_binding(resources.uniform_buffers[0].id));
    }
    PrintIndexPairs("srv", compiler, resources.separate_images);
    PrintIndexPairs("uav", compiler, resources.storage_images);
    PrintIndexPairs("sampler", compiler, resources.separate_samplers);

    return 0;
}
