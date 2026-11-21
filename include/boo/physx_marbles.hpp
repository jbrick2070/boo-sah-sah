#pragma once

// PhysX suite, the Explore marbles (docs/physx_suite_plan.md, step 2).
//
// The ten glass marbles Explore drops (Y / F10 / M) roll on PhysX's rigid-body solver on the CPU,
// with continuous collision detection so a fast marble never passes through the countertop. The
// collision world is the one the built-in marble solver in src/app/main.cpp has always used, built as
// PhysX shapes: in the dawn bathroom the countertop and its vessel basin (a height field sampled from
// the same surface), the floor, the walls, the walnut vanity and the four fixtures on the counter;
// elsewhere the ground and the altar. Builds without the PhysX SDK, or before the runtime is ready,
// keep the built-in solver.

#include "boo/physx_suite.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace boo {

struct MarbleState {
    float position[3]{};
    float velocity[3]{};
    float rotation[4]{0.0f, 0.0f, 0.0f, 1.0f}; // quaternion xyzw
    float radius = 0.02f;
};

class PhysxMarbles {
public:
    static constexpr uint32_t kMaxMarbles = 16;

    // `countertopHeight(x, z)` is the bathroom countertop surface the built-in solver uses.
    PhysxMarbles(PhysxSuite& runtime, std::function<float(float, float)> countertopHeight);
    ~PhysxMarbles();
    PhysxMarbles(const PhysxMarbles&) = delete;
    PhysxMarbles& operator=(const PhysxMarbles&) = delete;

    bool ok() const noexcept;
    // Replaces the marbles with these, in the bathroom's world or the altar's.
    void reset(bool dawnBathroom, const MarbleState* marbles, uint32_t count);
    // Advances by a frame's wall time at a fixed 1/240 s step; writes the marbles back. Returns
    // whether any marble is still moving.
    bool step(float wallSeconds, MarbleState* marbles, uint32_t count);
    uint64_t steps() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
