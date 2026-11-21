#pragma once

// PhysX suite, rigid bodies (docs/physx_suite_plan.md, step 2).
//
// A shower of enamel marbles over the city on PhysX's GPU rigid-body solver: an accent with its own
// moment, not a flood. A few thousand fall over a few seconds onto a block of real buildings (every
// building of the city being drawn is a static box, from the renderer's own placements), scatter and
// come to rest; half a minute later they rise and fall again. The birds keep their own flight.
//
// The scene exists only while the genre is dialled in. Results run a frame behind the simulation.

#include "boo/api.hpp"
#include "boo/physx_suite.hpp"

#include <memory>
#include <vector>

namespace boo {

class PhysxRigidGenre {
public:
    static constexpr uint32_t kMaxBodies = 3000;

    explicit PhysxRigidGenre(PhysxSuite& runtime);
    ~PhysxRigidGenre();
    PhysxRigidGenre(const PhysxRigidGenre&) = delete;
    PhysxRigidGenre& operator=(const PhysxRigidGenre&) = delete;

    // One frame. `buildings` and `buildingsAct` come from the renderer; `air` is the centre of the
    // birds' airspace (where the pour falls). A change of act rebuilds the city and clears the pour.
    void frame(double frameSeconds, bool paused, const std::vector<CityCollisionBox>& buildings, uint32_t buildingsAct,
               const float air[3], bool haveAir);

    const PhysxBodyFrame* bodies() const noexcept; // null until there is something to draw
    PhysxStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
