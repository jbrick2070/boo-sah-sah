#include "boo/baked_physics.hpp"

#include "boo/api.hpp"
#include "boo/startup_progress.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>

namespace boo {

BakedPose BakedTrack::sample(float time) const noexcept {
    if (keys.empty()) return {};
    const float at = std::clamp((time - firstTime) * keyRate, 0.0f, float(keys.size() - 1));
    const size_t i = std::min(static_cast<size_t>(at), keys.size() - 1);
    const size_t j = std::min(i + 1, keys.size() - 1);
    const float f = at - float(i);
    const BakedPose& a = keys[i];
    const BakedPose& b = keys[j];
    BakedPose out;
    for (int k = 0; k < 3; ++k) out.position[k] = a.position[k] + (b.position[k] - a.position[k]) * f;
    // Normalised lerp along the shorter arc: keys are 1/120 s apart, so this is as good as slerp.
    const float dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] + a.rotation[2] * b.rotation[2] + a.rotation[3] * b.rotation[3];
    const float sign = dot < 0.0f ? -1.0f : 1.0f;
    float length = 0.0f;
    for (int k = 0; k < 4; ++k) {
        out.rotation[k] = a.rotation[k] + (sign * b.rotation[k] - a.rotation[k]) * f;
        length += out.rotation[k] * out.rotation[k];
    }
    length = std::sqrt(std::max(length, 1e-12f));
    for (float& c : out.rotation) c /= length;
    return out;
}

void BakedGridTrack::sample(float time, float* out) const noexcept {
    const uint32_t keys = keyCount();
    if (!keys) { std::fill(out, out + size_t(nodes) * 3u, 0.0f); return; }
    const float at = std::clamp((time - firstTime) * keyRate, 0.0f, float(keys - 1));
    const uint32_t i = std::min(static_cast<uint32_t>(at), keys - 1);
    const float f = at - float(i);
    auto key = [&](int64_t k) { return offsets.data() + size_t(std::clamp<int64_t>(k, 0, keys - 1)) * nodes * 3u; };
    const int16_t* p0 = key(int64_t(i) - 1);
    const int16_t* p1 = key(i);
    const int16_t* p2 = key(int64_t(i) + 1);
    const int16_t* p3 = key(int64_t(i) + 2);
    // Catmull-Rom weights.
    const float f2 = f * f, f3 = f2 * f;
    const float w0 = 0.5f * (-f3 + 2.0f * f2 - f);
    const float w1 = 0.5f * (3.0f * f3 - 5.0f * f2 + 2.0f);
    const float w2 = 0.5f * (-3.0f * f3 + 4.0f * f2 + f);
    const float w3 = 0.5f * (f3 - f2);
    const size_t n = size_t(nodes) * 3u;
    for (size_t c = 0; c < n; ++c)
        out[c] = offsetScale * (w0 * float(p0[c]) + w1 * float(p1[c]) + w2 * float(p2[c]) + w3 * float(p3[c]));
}

const BakedTrack* BakedPhysics::find(const std::string& name) const noexcept {
    for (const auto& t : tracks) if (t.name == name) return &t;
    return nullptr;
}

const BakedGridTrack* BakedPhysics::findGrid(const std::string& name) const noexcept {
    for (const auto& t : grids) if (t.name == name) return &t;
    return nullptr;
}

bool loadBakedPhysics(const std::filesystem::path& path, BakedPhysics& out, std::string* error) {
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    std::ifstream file(path, std::ios::binary);
    if (!file) return fail("missing");
    char magic[4]{};
    uint32_t version = 0, count = 0;
    file.read(magic, 4);
    file.read(reinterpret_cast<char*>(&version), 4);
    file.read(reinterpret_cast<char*>(&count), 4);
    if (!file || std::memcmp(magic, "BPHY", 4) != 0 || (version != 1u && version != 2u) || count > 4096u) return fail("not a .bphys file");
    BakedPhysics loaded;
    for (uint32_t t = 0; t < count; ++t) {
        char name[32]{};
        BakedTrack track;
        uint32_t keys = 0;
        file.read(name, 32);
        file.read(reinterpret_cast<char*>(&track.firstTime), 4);
        file.read(reinterpret_cast<char*>(&track.keyRate), 4);
        file.read(reinterpret_cast<char*>(&keys), 4);
        if (!file || keys > (1u << 22) || !(track.keyRate > 0.0f)) return fail("bad track header");
        track.name.assign(name, strnlen(name, 32));
        track.keys.resize(keys);
        for (auto& k : track.keys) {
            file.read(reinterpret_cast<char*>(k.position.data()), 12);
            file.read(reinterpret_cast<char*>(k.rotation.data()), 16);
        }
        if (!file) return fail("truncated track");
        loaded.tracks.push_back(std::move(track));
    }
    if (version >= 2u) {
        uint32_t grids = 0;
        file.read(reinterpret_cast<char*>(&grids), 4);
        if (!file || grids > 256u) return fail("bad grid count");
        for (uint32_t g = 0; g < grids; ++g) {
            char name[32]{};
            BakedGridTrack grid;
            uint32_t keys = 0;
            file.read(name, 32);
            file.read(reinterpret_cast<char*>(&grid.firstTime), 4);
            file.read(reinterpret_cast<char*>(&grid.keyRate), 4);
            file.read(reinterpret_cast<char*>(&grid.offsetScale), 4);
            file.read(reinterpret_cast<char*>(&grid.nodes), 4);
            file.read(reinterpret_cast<char*>(&keys), 4);
            if (!file || !(grid.keyRate > 0.0f) || !(grid.offsetScale > 0.0f) || grid.nodes > (1u << 20) || uint64_t(keys) * grid.nodes > (1ull << 26))
                return fail("bad grid header");
            grid.name.assign(name, strnlen(name, 32));
            grid.offsets.resize(size_t(keys) * grid.nodes * 3u);
            file.read(reinterpret_cast<char*>(grid.offsets.data()), std::streamsize(grid.offsets.size() * sizeof(int16_t)));
            if (!file) return fail("truncated grid track");
            loaded.grids.push_back(std::move(grid));
        }
    }
    out = std::move(loaded);
    return true;
}

bool saveBakedPhysics(const std::filesystem::path& path, const BakedPhysics& physics, std::string* error) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) { if (error) *error = "cannot write"; return false; }
    const uint32_t version = physics.grids.empty() ? 1u : 2u, count = static_cast<uint32_t>(physics.tracks.size());
    file.write("BPHY", 4);
    file.write(reinterpret_cast<const char*>(&version), 4);
    file.write(reinterpret_cast<const char*>(&count), 4);
    for (const auto& t : physics.tracks) {
        char name[32]{};
        std::memcpy(name, t.name.data(), std::min<size_t>(t.name.size(), 31));
        const uint32_t keys = static_cast<uint32_t>(t.keys.size());
        file.write(name, 32);
        file.write(reinterpret_cast<const char*>(&t.firstTime), 4);
        file.write(reinterpret_cast<const char*>(&t.keyRate), 4);
        file.write(reinterpret_cast<const char*>(&keys), 4);
        for (const auto& k : t.keys) {
            file.write(reinterpret_cast<const char*>(k.position.data()), 12);
            file.write(reinterpret_cast<const char*>(k.rotation.data()), 16);
        }
    }
    if (version >= 2u) {
        const uint32_t grids = static_cast<uint32_t>(physics.grids.size());
        file.write(reinterpret_cast<const char*>(&grids), 4);
        for (const auto& g : physics.grids) {
            char name[32]{};
            std::memcpy(name, g.name.data(), std::min<size_t>(g.name.size(), 31));
            const uint32_t keys = g.keyCount();
            file.write(name, 32);
            file.write(reinterpret_cast<const char*>(&g.firstTime), 4);
            file.write(reinterpret_cast<const char*>(&g.keyRate), 4);
            file.write(reinterpret_cast<const char*>(&g.offsetScale), 4);
            file.write(reinterpret_cast<const char*>(&g.nodes), 4);
            file.write(reinterpret_cast<const char*>(&keys), 4);
            file.write(reinterpret_cast<const char*>(g.offsets.data()), std::streamsize(size_t(keys) * g.nodes * 3u * sizeof(int16_t)));
        }
    }
    return static_cast<bool>(file);
}

const BakedPhysics& filmBakedPhysics() {
    static BakedPhysics physics;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const char* name : {"assets/physics/parcel_winch.bphys", "assets/physics/iris_mechanism.bphys", "assets/physics/taipei_damper.bphys",
                                 "assets/physics/canopy_cloth.bphys", "assets/physics/wind_banner.bphys"}) {
            BakedPhysics part;
            const auto path = resolveResourcePath(name);
            if (loadBakedPhysics(path, part)) {
                startupAssetLoaded(path);
                for (auto& t : part.tracks) physics.tracks.push_back(std::move(t));
                for (auto& g : part.grids) physics.grids.push_back(std::move(g));
            }
        }
    });
    return physics;
}

} // namespace boo
