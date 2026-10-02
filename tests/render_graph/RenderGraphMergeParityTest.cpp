#include "application/TestCase.h"

#include "application/AppFramework.h"
#include "application/RenderFramework.h"
#include "core/ConfigManager.h"
#include "core/FileManager.h"
#include "core/Logger.h"
#include "rhi/RHI.h"

#include <nlohmann/json.hpp>
#include <stb_image.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>

namespace sparkle
{
// screenshots a frame and dumps the render graph of that same frame with render_graph_merge, render_graph_memoryless
// and render_graph_pixel_local on, then with memoryless off, then with pixel-local reads off, then with merging off,
// each switched at runtime: the screenshots must be bit-identical (the same pixels encode to the same PNG), while only
// the first three graphs merge passes, only the first and third may have memoryless transients, and only the first two
// read pixel-locally, on a device that can, where the first has memoryless transients too. Before the first capture,
// every configuration renders SettleFrames frames once, so no capture is among the first frames a pipeline draws,
// which a driver may render differently. A capture that differs from the first logs its pixel difference and is kept
// with the first under screenshots/captures/
class RenderGraphMergeParityTest : public TestCase
{
public:
    static constexpr uint32_t SettleFrames = 3;

    void OnEnforceConfigs() override
    {
        Apply(Captures.front());
    }

    Result OnTick(AppFramework &app) override
    {
        auto *render_framework = app.GetRenderFramework();
        if (capturing_)
        {
            if (!screenshot_->IsCompleted())
            {
                return Result::Pending;
            }

            capturing_ = false;
            if (step_ + 1 == Steps)
            {
                Compare(app.GetRHI()->SupportsPixelLocalRead());
                return HasFailed() ? Result::Fail : Result::Pass;
            }
            Advance();
            return Result::Pending;
        }

        if (!render_framework->IsReadyForAutoScreenshot() || ++ready_frames_ < SettleFrames)
        {
            return Result::Pending;
        }

        if (step_ < Captures.size())
        {
            Advance();
            return Result::Pending;
        }

        screenshot_ = render_framework->RequestTakeScreenshot(FileName(Captures[step_ % Captures.size()]), false, true);
        capturing_ = true;
        return Result::Pending;
    }

private:
    struct Capture
    {
        const char *name;
        bool merge;
        bool memoryless;
        bool pixel_local;
    };

    static constexpr std::array<Capture, 4> Captures{{
        {.name = "render_graph_merge_on", .merge = true, .memoryless = true, .pixel_local = true},
        {.name = "render_graph_memoryless_off", .merge = true, .memoryless = false, .pixel_local = true},
        {.name = "render_graph_pixel_local_off", .merge = true, .memoryless = true, .pixel_local = false},
        {.name = "render_graph_merge_off", .merge = false, .memoryless = true, .pixel_local = true},
    }};

    // each configuration renders once to warm up, then once to capture
    static constexpr size_t Steps = Captures.size() * 2;

    void Advance()
    {
        step_++;
        Apply(Captures[step_ % Captures.size()]);
        ready_frames_ = 0;
    }

    void Apply(const Capture &capture)
    {
        EnforceConfig("render_graph_merge", capture.merge);
        EnforceConfig("render_graph_memoryless", capture.memoryless);
        EnforceConfig("render_graph_pixel_local", capture.pixel_local);
    }

    [[nodiscard]] static std::string Read(const std::string &file)
    {
        return FileManager::GetNativeFileManager()->ReadAsType<std::string>(Path::External("screenshots/" + file));
    }

    [[nodiscard]] static std::string FileName(const Capture &capture)
    {
        return std::format("{}_{}", ConfigManager::Instance().GetConfig<std::string>("pipeline")->Get(), capture.name);
    }

    [[nodiscard]] static nlohmann::json ReadDump(const Capture &capture)
    {
        return nlohmann::json::parse(Read(FileName(capture) + ".json"));
    }

    static void Keep(const Capture &capture)
    {
        for (const auto *extension : {".png", ".json"})
        {
            const auto file = FileName(capture) + extension;
            const auto data = Read(file);
            FileManager::GetNativeFileManager()->Write(Path::External("screenshots/captures/" + file), data.data(),
                                                       data.size());
        }
    }

    [[nodiscard]] static auto Decode(const std::string &png, int &width, int &height)
    {
        int channels = 0;
        return std::unique_ptr<stbi_uc, decltype(&stbi_image_free)>(
            stbi_load_from_memory(reinterpret_cast<const stbi_uc *>(png.data()), static_cast<int>(png.size()), &width,
                                  &height, &channels, 4),
            stbi_image_free);
    }

    void LogDifference(const Capture &capture, const std::string &expected, const std::string &actual) const
    {
        int width = 0;
        int height = 0;
        int actual_width = 0;
        int actual_height = 0;
        const auto expected_pixels = Decode(expected, width, height);
        const auto actual_pixels = Decode(actual, actual_width, actual_height);
        if (!expected_pixels || !actual_pixels || width != actual_width || height != actual_height)
        {
            Log(Error, "{}: {} decodes to {}x{}, {} to {}x{}", GetName(), FileName(Captures.front()), width, height,
                FileName(capture), actual_width, actual_height);
            return;
        }

        std::array<int, 4> max_difference{};
        int differing = 0;
        int min_x = width;
        int min_y = height;
        int max_x = -1;
        int max_y = -1;
        for (int pixel = 0; pixel < width * height; pixel++)
        {
            bool differs = false;
            for (int channel = 0; channel < 4; channel++)
            {
                const auto index = (pixel * 4) + channel;
                const auto difference = std::abs(expected_pixels.get()[index] - actual_pixels.get()[index]);
                max_difference[channel] = std::max(max_difference[channel], difference);
                differs = differs || difference != 0;
            }
            if (differs)
            {
                differing++;
                min_x = std::min(min_x, pixel % width);
                min_y = std::min(min_y, pixel / width);
                max_x = std::max(max_x, pixel % width);
                max_y = std::max(max_y, pixel / width);
            }
        }
        Log(Error,
            "{}: {} differs from {} in {} of {}x{} pixels, max abs difference rgba ({}, {}, {}, {}), bounding box "
            "[{}, {}]-[{}, {}]",
            GetName(), FileName(capture), FileName(Captures.front()), differing, width, height, max_difference[0],
            max_difference[1], max_difference[2], max_difference[3], min_x, min_y, max_x, max_y);
    }

    [[nodiscard]] static bool Merges(const nlohmann::json &dump)
    {
        return std::ranges::any_of(dump.at("physical_passes"),
                                   [](const nlohmann::json &physical) { return physical.at("members").size() > 1; });
    }

    [[nodiscard]] static bool HasMemoryless(const nlohmann::json &dump)
    {
        return std::ranges::any_of(dump.at("resources"),
                                   [](const nlohmann::json &resource) { return resource.value("memoryless", false); });
    }

    [[nodiscard]] static bool ReadsPixelLocally(const nlohmann::json &dump)
    {
        return std::ranges::any_of(dump.at("passes"), [](const nlohmann::json &pass) {
            return std::ranges::any_of(pass.at("accesses"), [](const nlohmann::json &access) {
                return access.contains("pixel_local_slot") && !access.contains("lowered_reason");
            });
        });
    }

    void Compare(bool supports_pixel_local)
    {
        const auto first = Read(FileName(Captures.front()) + ".png");
        Expect(!first.empty(), "the first frame is saved");
        // without pixel-local reads, a frame may keep no transient within one physical pass
        Expect(!supports_pixel_local || HasMemoryless(ReadDump(Captures.front())),
               std::format("{} has memoryless transients", Captures.front().name));
        for (const auto &capture : Captures)
        {
            const auto dump = ReadDump(capture);
            Expect(Merges(dump) == capture.merge, std::format("{} merges passes only with merging", capture.name));
            Expect(!HasMemoryless(dump) || (capture.merge && capture.memoryless),
                   std::format("{} has memoryless transients only with merging and memoryless", capture.name));
            Expect(ReadsPixelLocally(dump) == (capture.merge && capture.pixel_local && supports_pixel_local),
                   std::format("{} reads pixel-locally only with merging and pixel-local reads", capture.name));
            const auto image = Read(FileName(capture) + ".png");
            const bool identical = image == first;
            Expect(identical, std::format("{} is bit-identical to {}", capture.name, Captures.front().name));
            if (!identical)
            {
                LogDifference(capture, first, image);
                Keep(Captures.front());
                Keep(capture);
            }
        }
    }

    size_t step_ = 0;
    bool capturing_ = false;
    std::shared_ptr<ScreenshotRequest> screenshot_;
    uint32_t ready_frames_ = 0;
};

static TestCaseRegistrar<RenderGraphMergeParityTest> merge_parity_test_registrar("render_graph_merge_parity");
} // namespace sparkle
