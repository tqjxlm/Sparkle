#include "renderer/nrd/NrdCookedShaders.h"

#include "core/FileManager.h"
#include "core/Logger.h"

#include <sstream>

namespace sparkle
{
namespace
{
#if FRAMEWORK_IOS
constexpr const char *CookedDir = "shaders/nrd/cooked/ios";
#else
constexpr const char *CookedDir = "shaders/nrd/cooked/macos";
#endif

bool ReadIndexList(std::istringstream &line, std::vector<uint32_t> &out)
{
    size_t count = 0;
    if (!(line >> count))
    {
        return false;
    }
    out.resize(count);
    for (auto &value : out)
    {
        int64_t raw = 0;
        if (!(line >> raw))
        {
            return false;
        }
        value = static_cast<uint32_t>(raw);
    }
    return true;
}
} // namespace

bool LoadNrdCookedShaders(NrdCookedShaders &out)
{
    auto *file_manager = FileManager::GetNativeFileManager();

    const std::string manifest_path = std::string(CookedDir) + "/manifest.txt";
    const auto manifest = file_manager->ReadAsType<std::string>(Path::Resource(manifest_path));
    if (manifest.empty())
    {
        Log(Error, "NRD: missing cooked shader manifest {}. run dev/cook_nrd_shaders.py", manifest_path);
        return false;
    }

    std::istringstream stream(manifest);
    std::string line;
    while (std::getline(stream, line))
    {
        std::istringstream tokens(line);
        std::string tag;
        tokens >> tag;

        if (tag == "nrd_version")
        {
            tokens >> out.version_major >> out.version_minor >> out.version_build;
        }
        else if (tag == "pipeline")
        {
            std::string identifier;
            std::string file_name;
            RHINrdBackend::CookedPipeline pipeline;
            int64_t cb_index = -1;
            tokens >> identifier >> file_name >> pipeline.entry_point >> pipeline.threads_per_group[0] >>
                pipeline.threads_per_group[1] >> pipeline.threads_per_group[2] >> cb_index;
            pipeline.constant_buffer_index = static_cast<uint32_t>(cb_index);

            const std::string msl_path = std::string(CookedDir) + "/" + file_name;
            pipeline.msl_source = file_manager->ReadAsType<std::string>(Path::Resource(msl_path));
            if (!tokens || pipeline.msl_source.empty())
            {
                Log(Error, "NRD: broken cooked shader entry '{}' ({})", identifier, msl_path);
                return false;
            }

            out.identifiers.push_back(std::move(identifier));
            out.pipelines.push_back(std::move(pipeline));
        }
        else if (tag == "srv" || tag == "uav" || tag == "sampler")
        {
            if (out.pipelines.empty())
            {
                return false;
            }
            auto &pipeline = out.pipelines.back();
            auto &indices = tag == "srv"   ? pipeline.srv_texture_indices
                            : tag == "uav" ? pipeline.uav_texture_indices
                                           : pipeline.sampler_indices;
            if (!ReadIndexList(tokens, indices))
            {
                Log(Error, "NRD: broken cooked manifest line: {}", line);
                return false;
            }
        }
    }

    return !out.pipelines.empty();
}
} // namespace sparkle
