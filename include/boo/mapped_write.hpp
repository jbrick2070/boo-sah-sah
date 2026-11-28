#pragma once

#include "boo/logging.hpp"
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_set>

namespace boo {

// Check before pointer arithmetic as well as before copying. Subtraction avoids
// overflowing offset + size when a bad count reaches this boundary.
inline bool mappedWriteFits(ID3D12Resource* resource, const char* name,
                            uint64_t offset, uint64_t size) {
    const uint64_t capacity = resource ? resource->GetDesc().Width : 0;
    if (resource && offset <= capacity && size <= capacity - offset) return true;
    static std::mutex warningMutex;
    static std::unordered_set<std::string> warned;
    const std::lock_guard lock(warningMutex);
    if (warned.emplace(name).second) {
        BOO_LOG_WARN("D3D12", std::string("Mapped write out of bounds: ") + name +
            " offset=" + std::to_string(offset) + " size=" + std::to_string(size) +
            " capacity=" + std::to_string(capacity));
    }
    return false;
}

} // namespace boo

// The failure path logs once per buffer name, even if it recurs every frame.
#define BOO_MAPPED_WRITE_OK(resource, name, offset, size) \
    (::boo::mappedWriteFits((resource), (name), (offset), (size)))
