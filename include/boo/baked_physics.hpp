#pragma once

// Baked film physics (docs/physx_suite_plan.md, step 4). The PhysX bake tool (tools/physx_bake,
// test-rig builds) simulates offline at a fixed step and writes tracks of poses against film time into
// assets/physics/*.bphys. The film plays them back here as pure functions of film time: seek-safe,
// identical every run, and needing no physics library, so the Store build plays them too.
//
// File layout (little-endian): "BPHY", u32 version (1 or 2), u32 track count, then for each track:
// char[32] name (zero padded), f32 first key time (s), f32 key rate (Hz), u32 key count, then
// count x { f32 position xyz, f32 rotation quaternion xyzw }.
// Version 2 adds, after the rigid tracks, u32 grid track count, then for each: char[32] name,
// f32 first key time, f32 key rate, f32 offset scale (metres per unit), u32 node count, u32 key count,
// then keys x nodes x i16 offset xyz from the rest shape. Version 1 files are written while no grid
// track is present, so the existing files stay byte for byte as they were.

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace boo {

struct BakedPose {
    std::array<float, 3> position{};
    std::array<float, 4> rotation{0.0f, 0.0f, 0.0f, 1.0f}; // quaternion x, y, z, w
};

struct BakedTrack {
    std::string name;
    float firstTime = 0.0f;
    float keyRate = 120.0f;
    std::vector<BakedPose> keys;

    float lastTime() const noexcept { return keys.empty() ? firstTime : firstTime + float(keys.size() - 1) / keyRate; }
    bool covers(float time) const noexcept { return !keys.empty() && time >= firstTime && time <= lastTime(); }
    // Interpolated pose; clamped to the first and last keys outside the track.
    BakedPose sample(float time) const noexcept;
};

// A deforming surface: per key, the offset of every node of a grid from its rest position.
struct BakedGridTrack {
    std::string name;
    float firstTime = 0.0f;
    float keyRate = 10.0f;
    float offsetScale = 1.0f / 2000.0f;
    uint32_t nodes = 0;
    std::vector<int16_t> offsets; // keys x nodes x 3

    uint32_t keyCount() const noexcept { return nodes ? static_cast<uint32_t>(offsets.size() / (size_t(nodes) * 3u)) : 0u; }
    float lastTime() const noexcept { const uint32_t k = keyCount(); return k ? firstTime + float(k - 1) / keyRate : firstTime; }
    bool covers(float time) const noexcept { return keyCount() > 0 && time >= firstTime && time <= lastTime(); }
    // Offsets at `time` in metres, xyz per node, with Catmull-Rom interpolation between keys (clamped
    // to the first and last keys). `out` must hold nodes x 3 floats.
    void sample(float time, float* out) const noexcept;
};

struct BakedPhysics {
    std::vector<BakedTrack> tracks;
    std::vector<BakedGridTrack> grids;
    const BakedTrack* find(const std::string& name) const noexcept;
    const BakedGridTrack* findGrid(const std::string& name) const noexcept;
};

bool loadBakedPhysics(const std::filesystem::path& path, BakedPhysics& out, std::string* error = nullptr);
bool saveBakedPhysics(const std::filesystem::path& path, const BakedPhysics& physics, std::string* error = nullptr);

// The film's baked physics, loaded once from assets/physics (empty if the files are absent).
const BakedPhysics& filmBakedPhysics();

} // namespace boo
