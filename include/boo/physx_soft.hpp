#pragma once

// PhysX suite, soft bodies (docs/physx_suite_plan.md, step 6, live). Two dozen beneficial microbes,
// soft celadon blobs, fall onto one block of the city on PhysX's GPU finite-element solver for
// deformable volumes (tetrahedral meshes): they squash on the roofs and ledges, wobble, and slump
// into the gaps between buildings; half a minute after they settle they rise and fall again. Each is
// drawn as the ellipsoid that best fits its simulated shape (centre, principal axes and extents of its
// collision mesh, read back from the GPU every frame), through the same instance path as the rigid
// bodies, so the squash and stretch are the solver's own.

#include "boo/api.hpp"
#include "boo/physx_suite.hpp"

#include <memory>
#include <vector>

namespace boo {

class PhysxSoftGenre {
public:
    static constexpr uint32_t kMicrobes = 24;

    explicit PhysxSoftGenre(PhysxSuite& runtime);
    ~PhysxSoftGenre();
    PhysxSoftGenre(const PhysxSoftGenre&) = delete;
    PhysxSoftGenre& operator=(const PhysxSoftGenre&) = delete;

    void frame(double frameSeconds, bool paused, const std::vector<CityCollisionBox>& buildings, uint32_t buildingsAct,
               const float air[3], bool haveAir);
    const PhysxBodyFrame* bodies() const noexcept;
    PhysxStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace boo
