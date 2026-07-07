#include "renderer/nrd/NrdCookedShaders.h"

#include "core/FileManager.h"
#include "core/Logger.h"

#include <NRD.h>

#include <algorithm>
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

// must produce the same stem as shaders/nrd/cook/cook_nrd_shaders.py: defines sorted by name
// (shaderIdentifier lists them in NRD's declaration order, the cooked blob keys sort them),
// non-filename characters replaced with '_'
std::string CanonicalStem(const std::string &identifier)
{
    std::vector<std::string> parts;
    std::istringstream stream(identifier);
    std::string part;
    while (std::getline(stream, part, '|'))
    {
        parts.push_back(part);
    }
    std::sort(parts.begin() + 1, parts.end());

    std::string stem;
    for (size_t i = 0; i < parts.size(); i++)
    {
        stem += (i == 0 ? "" : "|") + parts[i];
    }
    for (auto &c : stem)
    {
        if (isalnum(static_cast<unsigned char>(c)) == 0 && c != '.' && c != '-')
        {
            c = '_';
        }
    }
    return stem;
}

bool ReadIndexPairs(std::istringstream &line, std::vector<std::pair<uint32_t, uint32_t>> &out)
{
    size_t count = 0;
    if (!(line >> count))
    {
        return false;
    }
    out.resize(count);
    for (auto &[binding, msl_index] : out)
    {
        if (!(line >> binding >> msl_index))
        {
            return false;
        }
    }
    return true;
}

void ResolveRegisters(const std::vector<std::pair<uint32_t, uint32_t>> &pairs, uint32_t register_offset,
                      std::vector<uint32_t> &out)
{
    for (const auto &[binding, msl_index] : pairs)
    {
        const uint32_t reg = binding - register_offset;
        if (reg >= out.size())
        {
            out.resize(reg + 1, ~0u);
        }
        out[reg] = msl_index;
    }
}
} // namespace

bool NrdCookedShaders::Load()
{
    auto *file_manager = FileManager::GetNativeFileManager();

    const std::string manifest_path = std::string(CookedDir) + "/manifest.txt";
    const auto manifest = file_manager->ReadAsType<std::string>(Path::Resource(manifest_path));
    if (manifest.empty())
    {
        Log(Error, "NRD: missing cooked shader manifest {}", manifest_path);
        return false;
    }

    std::istringstream stream(manifest);
    std::string line;
    Entry *current = nullptr;
    while (std::getline(stream, line))
    {
        std::istringstream tokens(line);
        std::string tag;
        tokens >> tag;

        if (tag == "nrd_version")
        {
            tokens >> version_major_ >> version_minor_ >> version_build_;
        }
        else if (tag == "pipeline")
        {
            std::string stem;
            Entry entry;
            int64_t cb_index = -1;
            tokens >> stem >> entry.file_name >> entry.entry_point >> entry.threads_per_group[0] >>
                entry.threads_per_group[1] >> entry.threads_per_group[2] >> cb_index;
            entry.constant_buffer_index = static_cast<uint32_t>(cb_index);
            if (!tokens)
            {
                Log(Error, "NRD: broken cooked manifest line: {}", line);
                return false;
            }
            current = &entries_[stem];
            *current = std::move(entry);
        }
        else if (tag == "srv" || tag == "uav" || tag == "sampler")
        {
            if (current == nullptr)
            {
                return false;
            }
            auto &pairs = tag == "srv" ? current->srv : tag == "uav" ? current->uav : current->samplers;
            if (!ReadIndexPairs(tokens, pairs))
            {
                Log(Error, "NRD: broken cooked manifest line: {}", line);
                return false;
            }
        }
    }

    return !entries_.empty();
}

bool NrdCookedShaders::BuildPipeline(const char *shader_identifier, RHINrdBackend::CookedPipeline &out) const
{
    const std::string stem = CanonicalStem(shader_identifier);
    auto found = entries_.find(stem);
    if (found == entries_.end())
    {
        Log(Error, "NRD: shader '{}' ({}) was not cooked; extend the filters in shaders/CMakeLists.txt",
            shader_identifier, stem);
        return false;
    }
    const Entry &entry = found->second;

    const std::string msl_path = std::string(CookedDir) + "/" + entry.file_name;
    out.msl_source = FileManager::GetNativeFileManager()->ReadAsType<std::string>(Path::Resource(msl_path));
    if (out.msl_source.empty())
    {
        Log(Error, "NRD: missing cooked shader file {}", msl_path);
        return false;
    }

    out.entry_point = entry.entry_point;
    std::copy(std::begin(entry.threads_per_group), std::end(entry.threads_per_group),
              std::begin(out.threads_per_group));
    out.constant_buffer_index = entry.constant_buffer_index;

    const auto &offsets = nrd::GetLibraryDesc()->spirvBindingOffsets;
    ResolveRegisters(entry.srv, offsets.textureOffset, out.srv_texture_indices);
    ResolveRegisters(entry.uav, offsets.storageTextureAndBufferOffset, out.uav_texture_indices);
    ResolveRegisters(entry.samplers, offsets.samplerOffset, out.sampler_indices);

    return true;
}
} // namespace sparkle
