#if FRAMEWORK_MACOS

#include "application/TestCase.h"

#include "core/ConfigValue.h"
#include "core/Logger.h"

#include "NRD.h"
#include "spirv_cross/spirv_msl.hpp"

#include <filesystem>
#include <format>
#include <fstream>

namespace sparkle
{
static ConfigValue<std::string> config_cook_output("nrd_cook_output", "output directory for nrd_cook", "test",
                                                   "shaders/nrd/cooked", false);

namespace
{
struct CookedReflection
{
    std::string entry;
    uint32_t wg[3];
    uint32_t cb_index = ~0u;
    std::vector<uint32_t> srv_indices;
    std::vector<uint32_t> uav_indices;
    std::vector<uint32_t> sampler_indices;
};

std::string SanitizeFileName(const std::string &name)
{
    std::string out = name;
    for (auto &c : out)
    {
        if (isalnum(static_cast<unsigned char>(c)) == 0 && c != '.' && c != '-')
        {
            c = '_';
        }
    }
    return out;
}

std::string IndexList(const std::vector<uint32_t> &indices)
{
    std::string out = std::to_string(indices.size());
    for (auto i : indices)
    {
        out += " " + std::to_string(static_cast<int32_t>(i));
    }
    return out;
}
} // namespace

// Cross-compiles every ReBLUR pipeline SPIR-V->MSL for macOS AND iOS and writes the results (plus a
// manifest with entry/workgroup/binding metadata) into shaders/nrd/cooked/, which is committed and
// packed into app resources. This moves spirv-cross out of the runtime entirely: the app loads the
// cooked MSL like any other shader. Re-run (dev/cook_nrd_shaders.py) whenever the NRD submodule
// changes; NrdDenoiser hard-fails on a manifest version mismatch.
// Usage: --test_case nrd_cook (run from the project root; writes into the source tree)
class NrdCookTest : public TestCase
{
public:
    Result OnTick(AppFramework &) override
    {
        nrd::DenoiserDesc denoiser{};
        denoiser.identifier = 0;
        denoiser.denoiser = nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR;
        nrd::InstanceCreationDesc creation{};
        creation.denoisers = &denoiser;
        creation.denoisersNum = 1;
        nrd::Instance *instance = nullptr;
        if (nrd::CreateInstance(creation, instance) != nrd::Result::SUCCESS)
        {
            Log(Error, "{}: nrd::CreateInstance failed", GetName());
            return Result::Fail;
        }

        const nrd::InstanceDesc &desc = *nrd::GetInstanceDesc(*instance);
        const nrd::LibraryDesc &lib = *nrd::GetLibraryDesc();

        bool ok = true;
        for (const auto &[platform, dir_name] :
             {std::pair{spirv_cross::CompilerMSL::Options::macOS, "macos"},
              std::pair{spirv_cross::CompilerMSL::Options::iOS, "ios"}})
        {
            const std::filesystem::path out_dir = std::filesystem::path(config_cook_output.Get()) / dir_name;
            std::filesystem::remove_all(out_dir);
            std::filesystem::create_directories(out_dir);

            std::ofstream manifest(out_dir / "manifest.txt");
            manifest << std::format("nrd_version {} {} {}\n", lib.versionMajor, lib.versionMinor, lib.versionBuild);

            for (uint32_t i = 0; i < desc.pipelinesNum; i++)
            {
                const nrd::PipelineDesc &pipeline = desc.pipelines[i];
                CookedReflection reflection;
                std::string msl;
                if (!CrossCompile(pipeline, platform, msl, reflection))
                {
                    Log(Error, "{}: [{}] {} failed to cross-compile", GetName(), i, pipeline.shaderIdentifier);
                    ok = false;
                    continue;
                }

                const std::string file_name = SanitizeFileName(std::format("{}_{}.metal", i, pipeline.shaderIdentifier));
                std::ofstream(out_dir / file_name) << msl;

                manifest << std::format("pipeline {} {} {} {} {} {} {}\n", pipeline.shaderIdentifier, file_name,
                                        reflection.entry, reflection.wg[0], reflection.wg[1], reflection.wg[2],
                                        static_cast<int32_t>(reflection.cb_index));
                manifest << "srv " << IndexList(reflection.srv_indices) << "\n";
                manifest << "uav " << IndexList(reflection.uav_indices) << "\n";
                manifest << "sampler " << IndexList(reflection.sampler_indices) << "\n";
            }

            Log(Info, "{}: cooked {} pipelines -> {}", GetName(), desc.pipelinesNum, out_dir.string());
        }

        nrd::DestroyInstance(*instance);
        return ok ? Result::Pass : Result::Fail;
    }

private:
    static bool CrossCompile(const nrd::PipelineDesc &pipeline, spirv_cross::CompilerMSL::Options::Platform platform,
                             std::string &msl, CookedReflection &reflection)
    {
        const auto *spirv = static_cast<const uint32_t *>(pipeline.computeShaderSPIRV.bytecode);
        const size_t words = static_cast<size_t>(pipeline.computeShaderSPIRV.size) / 4u;

        // must match the options MetalShader/MetalNrdBackend compile the generated source with
        spirv_cross::CompilerMSL compiler(spirv, words);
        spirv_cross::CompilerMSL::Options options;
        options.set_msl_version(3, 0, 0);
        options.platform = platform;
        compiler.set_msl_options(options);
        msl = compiler.compile();
        if (msl.empty())
        {
            return false;
        }

        const auto &entry_points = compiler.get_entry_points_and_stages();
        reflection.entry =
            compiler.get_cleansed_entry_point_name(entry_points[0].name, entry_points[0].execution_model);

        const auto &entry_point = compiler.get_entry_point(entry_points[0].name, entry_points[0].execution_model);
        reflection.wg[0] = entry_point.workgroup_size.x;
        reflection.wg[1] = entry_point.workgroup_size.y;
        reflection.wg[2] = entry_point.workgroup_size.z;

        const spirv_cross::ShaderResources resources = compiler.get_shader_resources();
        const auto &offsets = nrd::GetLibraryDesc()->spirvBindingOffsets;
        auto collect = [&](const auto &list, uint32_t offset) {
            std::vector<uint32_t> indices;
            for (const auto &resource : list)
            {
                const uint32_t reg = compiler.get_decoration(resource.id, spv::DecorationBinding) - offset;
                if (reg >= indices.size())
                {
                    indices.resize(reg + 1, ~0u);
                }
                indices[reg] = compiler.get_automatic_msl_resource_binding(resource.id);
            }
            return indices;
        };

        reflection.srv_indices = collect(resources.separate_images, offsets.textureOffset);
        reflection.uav_indices = collect(resources.storage_images, offsets.storageTextureAndBufferOffset);
        reflection.sampler_indices = collect(resources.separate_samplers, offsets.samplerOffset);
        if (!resources.uniform_buffers.empty())
        {
            reflection.cb_index = compiler.get_automatic_msl_resource_binding(resources.uniform_buffers[0].id);
        }

        return true;
    }
};

static TestCaseRegistrar<NrdCookTest> nrd_cook_registrar("nrd_cook");
} // namespace sparkle

#endif
