#pragma once

#include "core/cook/CookArtifact.h"

namespace sparkle
{
// Validated persistence for disposable derived data. The store owns manifest and
// domain lookup policy; it does not schedule or execute cook jobs.
class CookArtifactStore
{
public:
    // Resolves packaged content before the writable internal cache. An unresolved key
    // uses its logical identity; a resolved key may reuse identical relocated content.
    [[nodiscard]] static CookPayload Load(const CookArtifactKey &key);

    // Only resolved, non-empty outputs can be persisted.
    static bool Save(const CookArtifactKey &key, const CookPayload &payload);
};
} // namespace sparkle
