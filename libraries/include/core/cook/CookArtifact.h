#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace sparkle
{
using CookPayload = std::vector<char>;

// Logical identity is type/version/name. The hash is absent only while resolving a
// logical request; job-created keys always carry a hash, including a valid hash of zero.
struct CookArtifactKey
{
    std::string type;
    uint32_t version = 0;
    std::string source_name;
    std::optional<uint32_t> source_hash;
};

class CookJobResult
{
public:
    enum class Status : uint8_t
    {
        Succeeded,
        Failed,
        Unsupported,
    };

    [[nodiscard]] static CookJobResult Success(CookPayload payload)
    {
        return CookJobResult(Status::Succeeded, std::move(payload));
    }

    [[nodiscard]] static CookJobResult Failure()
    {
        return CookJobResult(Status::Failed, {});
    }

    [[nodiscard]] static CookJobResult Unsupported()
    {
        return CookJobResult(Status::Unsupported, {});
    }

    [[nodiscard]] Status GetStatus() const
    {
        return status_;
    }

    [[nodiscard]] bool IsSuccess() const
    {
        return status_ == Status::Succeeded;
    }

    [[nodiscard]] bool IsUnsupported() const
    {
        return status_ == Status::Unsupported;
    }

    [[nodiscard]] const CookPayload &GetPayload() const
    {
        return payload_;
    }

    [[nodiscard]] CookPayload TakePayload()
    {
        return std::move(payload_);
    }

private:
    CookJobResult(Status status, CookPayload payload) : status_(status), payload_(std::move(payload))
    {
    }

    Status status_;
    CookPayload payload_;
};
} // namespace sparkle
