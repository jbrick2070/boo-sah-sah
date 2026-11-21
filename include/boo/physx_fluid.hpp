#pragma once

// PhysX suite, fluids (docs/physx_suite_plan.md, step 6, live part). A small, contained cloudburst over one block of the city on
// PhysX's GPU position-based fluid solver (PBD particle system): water released above the birds'
// airspace falls in the shared terrace wind, pours off the real buildings and pools in the streets,
// then rises again as fresh rain. Each particle is drawn as a small glass bead through the same
// instance path as the rigid bodies. (The film's reflecting pool, baked as a ripple heightfield, waits
// for the deforming-surface renderer.)

#include "boo/api.hpp"
#include "boo/physx_suite.hpp"

#include <memory>
#include <vector>

namespace boo {

class PhysxFluidGenre {
public:
    static constexpr uint32_t kMaxParticles = 8000;

    explicit PhysxFluidGenre(PhysxSuite& runtime);
    ~PhysxFluidGenre();
    PhysxFluidGenre(const PhysxFluidGenre&) = delete;
    PhysxFluidGenre& operator=(const PhysxFluidGenre&) = delete;

    void frame(double frameSeconds, double filmSeconds, bool paused, const std::vector<CityCollisionBox>& buildings,
               uint32_t buildingsAct, const float air[3], bool haveAir);
    const PhysxBodyFrame* bodies() const noexcept;
    PhysxStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
