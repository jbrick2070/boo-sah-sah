#include "boo/physx_marbles.hpp"

#include "boo/logging.hpp"
#include "boo/physx_stepper.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(BOO_HAS_PHYSX)
#pragma warning(push, 0)
#include "PxPhysicsAPI.h"
#pragma warning(pop)
#endif

namespace boo {

struct PhysxMarbles::Impl {
    Impl(PhysxSuite& r, std::function<float(float, float)> h) : runtime(r), countertop(std::move(h)) {}
    PhysxSuite& runtime;
    std::function<float(float, float)> countertop;
    uint64_t steps = 0;
#if defined(BOO_HAS_PHYSX)
    physx::PxScene* scene = nullptr;
    physx::PxMaterial* stone = nullptr;
    physx::PxMaterial* glass = nullptr;
    std::vector<physx::PxRigidActor*> statics;
    std::vector<physx::PxRigidDynamic*> bodies;
    PhysxStepper stepper{1.0 / 240.0, 8u};
    int world = -1; // 0 altar, 1 bathroom
    void releaseBodies();
    void buildWorld(bool bathroom);
#endif
};

#if defined(BOO_HAS_PHYSX)
using namespace physx;

namespace {
// Contacts as the default shader makes them, plus continuous collision detection.
PxFilterFlags marbleFilter(PxFilterObjectAttributes a0, PxFilterData, PxFilterObjectAttributes a1, PxFilterData, PxPairFlags& flags, const void*, PxU32) {
    if (PxFilterObjectIsTrigger(a0) || PxFilterObjectIsTrigger(a1)) { flags = PxPairFlag::eTRIGGER_DEFAULT; return PxFilterFlag::eDEFAULT; }
    flags = PxPairFlag::eCONTACT_DEFAULT | PxPairFlag::eDETECT_CCD_CONTACT;
    return PxFilterFlag::eDEFAULT;
}
} // namespace

void PhysxMarbles::Impl::releaseBodies() {
    for (auto* b : bodies) { scene->removeActor(*b); b->release(); }
    bodies.clear();
}

void PhysxMarbles::Impl::buildWorld(bool bathroom) {
    auto* physics = static_cast<PxPhysics*>(runtime.physics());
    for (auto* s : statics) { scene->removeActor(*s); s->release(); }
    statics.clear();
    auto add = [&](PxRigidStatic* actor) { scene->addActor(*actor); statics.push_back(actor); };
    auto box = [&](float x0, float y0, float z0, float x1, float y1, float z1) {
        add(PxCreateStatic(*physics, PxTransform(PxVec3((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f)),
                           PxBoxGeometry((x1 - x0) * 0.5f, (y1 - y0) * 0.5f, (z1 - z0) * 0.5f), *stone));
    };
    // A plane facing along `normal` through `point`.
    auto plane = [&](PxVec3 normal, PxVec3 point) { add(PxCreatePlane(*physics, PxPlane(point, normal), *stone)); };
    if (!bathroom) {
        plane(PxVec3(0, 1, 0), PxVec3(0, 0, 0));
        box(-0.58f, 0.0f, 5.70f, 0.58f, 2.16f, 6.86f); // the altar
        world = 0;
        return;
    }
    plane(PxVec3(0, 1, 0), PxVec3(0, 2.650f, 0));       // floor
    plane(PxVec3(0, 0, -1), PxVec3(0, 0, 6.555f));      // back wall
    plane(PxVec3(0, 0, 1), PxVec3(0, 0, 2.300f));       // front of the room
    plane(PxVec3(1, 0, 0), PxVec3(-2.440f, 0, 0));      // side walls
    plane(PxVec3(-1, 0, 0), PxVec3(1.480f, 0, 0));
    box(-0.885f, 2.960f, 6.010f, 0.505f, 3.535f, 6.555f); // the walnut vanity under the counter
    // The countertop and its vessel basin: a height field sampled every 4 mm from the same surface.
    {
        constexpr float x0 = -0.895f, x1 = 0.515f, z0 = 5.990f, z1 = 6.580f, base = 3.50f, scale = 1.0e-5f, cell = 0.004f;
        const PxU32 rows = PxU32(std::ceil((x1 - x0) / cell)) + 1u, cols = PxU32(std::ceil((z1 - z0) / cell)) + 1u;
        std::vector<PxHeightFieldSample> samples(size_t(rows) * cols);
        for (PxU32 r = 0; r < rows; ++r)
            for (PxU32 c = 0; c < cols; ++c) {
                const float x = std::min(x1, x0 + float(r) * cell), z = std::min(z1, z0 + float(c) * cell);
                auto& s = samples[size_t(r) * cols + c];
                s.height = PxI16(std::clamp(std::lround((countertop(x, z) - base) / scale), -32767l, 32767l));
                s.materialIndex0 = s.materialIndex1 = 0;
            }
        PxHeightFieldDesc desc;
        desc.nbRows = rows;
        desc.nbColumns = cols;
        desc.samples.data = samples.data();
        desc.samples.stride = sizeof(PxHeightFieldSample);
        if (PxHeightField* field = PxCreateHeightField(desc, physics->getPhysicsInsertionCallback())) {
            add(PxCreateStatic(*physics, PxTransform(PxVec3(x0, base, z0)), PxHeightFieldGeometry(field, PxMeshGeometryFlags(), scale, cell, cell), *stone));
            field->release();
        }
    }
    // The four fixtures and glass props on the counter, as upright cylinders 0.36 m tall.
    {
        struct Fixture { float x, z, r; };
        constexpr Fixture kFixtures[4] = {{-0.180f, 6.472f, 0.030f}, {0.240f, 6.320f, 0.045f}, {0.395f, 6.250f, 0.046f}, {-0.590f, 6.265f, 0.116f}};
        for (const auto& f : kFixtures) {
            std::vector<PxVec3> ring;
            for (int i = 0; i < 24; ++i) {
                const float a = float(i) * (2.0f * PxPi / 24.0f);
                ring.emplace_back(f.r * std::cos(a), -0.18f, f.r * std::sin(a));
                ring.emplace_back(f.r * std::cos(a), 0.18f, f.r * std::sin(a));
            }
            PxConvexMeshDesc desc;
            desc.points.count = PxU32(ring.size());
            desc.points.stride = sizeof(PxVec3);
            desc.points.data = ring.data();
            desc.flags = PxConvexFlag::eCOMPUTE_CONVEX;
            PxCookingParams cooking(physics->getTolerancesScale());
            if (PxConvexMesh* mesh = PxCreateConvexMesh(cooking, desc, physics->getPhysicsInsertionCallback())) {
                add(PxCreateStatic(*physics, PxTransform(PxVec3(f.x, 3.70f, f.z)), PxConvexMeshGeometry(mesh), *stone));
                mesh->release();
            }
        }
    }
    world = 1;
}
#endif

PhysxMarbles::PhysxMarbles(PhysxSuite& runtime, std::function<float(float, float)> countertopHeight)
    : impl_(std::make_unique<Impl>(runtime, std::move(countertopHeight))) {
#if defined(BOO_HAS_PHYSX)
    auto& s = *impl_;
    auto* physics = static_cast<PxPhysics*>(runtime.physics());
    auto* dispatcher = static_cast<PxCpuDispatcher*>(runtime.dispatcher());
    if (!physics || !dispatcher) return;
    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = marbleFilter;
    desc.flags |= PxSceneFlag::eENABLE_CCD;
    desc.bounceThresholdVelocity = 0.22f; // slower impacts do not bounce, as the built-in solver had it
    s.scene = physics->createScene(desc);
    // Honed stone and glass: glass on stone keeps 0.56 of its speed, glass on glass 0.82.
    s.stone = physics->createMaterial(0.35f, 0.30f, 0.56f);
    s.glass = physics->createMaterial(0.30f, 0.25f, 0.82f);
    s.stone->setRestitutionCombineMode(PxCombineMode::eMIN);
    s.glass->setRestitutionCombineMode(PxCombineMode::eMIN);
    BOO_LOG_INFO("PhysX", "Explore marbles on PhysX rigid bodies (CPU, continuous collision detection)");
#endif
}

PhysxMarbles::~PhysxMarbles() {
#if defined(BOO_HAS_PHYSX)
    auto& s = *impl_;
    if (!s.scene) return;
    s.releaseBodies();
    for (auto* a : s.statics) { s.scene->removeActor(*a); a->release(); }
    s.statics.clear();
    s.scene->release();
    s.stone->release();
    s.glass->release();
#endif
}

bool PhysxMarbles::ok() const noexcept {
#if defined(BOO_HAS_PHYSX)
    return impl_->scene != nullptr;
#else
    return false;
#endif
}

uint64_t PhysxMarbles::steps() const noexcept { return impl_->steps; }

void PhysxMarbles::reset(bool dawnBathroom, const MarbleState* marbles, uint32_t count) {
#if defined(BOO_HAS_PHYSX)
    auto& s = *impl_;
    if (!s.scene) return;
    auto* physics = static_cast<PxPhysics*>(s.runtime.physics());
    s.releaseBodies();
    if (s.world != (dawnBathroom ? 1 : 0)) s.buildWorld(dawnBathroom);
    s.stepper.advance(0.0, false, true);
    for (uint32_t i = 0; i < std::min(count, kMaxMarbles); ++i) {
        const MarbleState& m = marbles[i];
        const PxQuat q(m.rotation[0], m.rotation[1], m.rotation[2], m.rotation[3]);
        PxRigidDynamic* body = PxCreateDynamic(*physics, PxTransform(PxVec3(m.position[0], m.position[1], m.position[2]), q.getNormalized()),
                                               PxSphereGeometry(m.radius), *s.glass, 2500.0f);
        body->setLinearVelocity(PxVec3(m.velocity[0], m.velocity[1], m.velocity[2]));
        body->setRigidBodyFlag(PxRigidBodyFlag::eENABLE_CCD, true);
        body->setLinearDamping(0.18f);  // air
        // Rolling resistance on honed stone. A rolling sphere feels angular damping through friction at
        // 2/7 strength, so 2.9 slows it as the built-in solver's 0.82 a second did.
        body->setAngularDamping(2.9f);
        body->setSleepThreshold(0.0125f);
        body->setSolverIterationCounts(8, 2);
        s.scene->addActor(*body);
        s.bodies.push_back(body);
    }
#else
    (void)dawnBathroom; (void)marbles; (void)count;
#endif
}

bool PhysxMarbles::step(float wallSeconds, MarbleState* marbles, uint32_t count) {
#if defined(BOO_HAS_PHYSX)
    auto& s = *impl_;
    if (!s.scene) return false;
    const uint32_t n = s.stepper.advance(std::clamp(double(wallSeconds), 0.0, 0.1), false, false);
    for (uint32_t i = 0; i < n; ++i) {
        s.scene->simulate(PxReal(s.stepper.stepSeconds()));
        s.scene->fetchResults(true);
        ++s.steps;
    }
    bool moving = false;
    for (uint32_t i = 0; i < std::min<uint32_t>(count, uint32_t(s.bodies.size())); ++i) {
        const PxTransform pose = s.bodies[i]->getGlobalPose();
        const PxVec3 v = s.bodies[i]->getLinearVelocity();
        MarbleState& m = marbles[i];
        m.position[0] = pose.p.x; m.position[1] = pose.p.y; m.position[2] = pose.p.z;
        m.velocity[0] = v.x; m.velocity[1] = v.y; m.velocity[2] = v.z;
        m.rotation[0] = pose.q.x; m.rotation[1] = pose.q.y; m.rotation[2] = pose.q.z; m.rotation[3] = pose.q.w;
        moving = moving || !s.bodies[i]->isSleeping();
    }
    return moving;
#else
    (void)wallSeconds; (void)marbles; (void)count;
    return false;
#endif
}

} // namespace boo
