#pragma once

#include <filesystem>
#include <functional>

namespace boo::detail {
// Stage beside the destination and publish only after the complete write succeeds.
bool publishResultsFile(const std::filesystem::path& destination,
                        const std::function<bool(const std::filesystem::path&)>& stage);
}
