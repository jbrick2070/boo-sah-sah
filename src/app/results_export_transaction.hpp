#pragma once

#include <filesystem>
#include <functional>

namespace boo::detail {
// Publishes a completed pair by renaming its private staging directory.
// Used by the exporter and its partial-write rollback regression.
bool publishResultsPair(const std::filesystem::path& destination,
                        const std::function<bool(const std::filesystem::path&)>& stage);
}
