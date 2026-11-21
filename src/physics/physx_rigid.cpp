#include "boo/physx_rigid.hpp"

#include "boo/logging.hpp"
#include "boo/physx_genres.hpp"
#include "boo/physx_stepper.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#if defined(BOO_HAS_PHYSX)
#pragma warning(push, 0)
#include "PxPhysicsAPI.h"
#pragma warning(pop)
#endif

namespace boo {

struct PhysxRigidGenre::Impl {
    explicit Impl(PhysxSuite& r) : runtime(r) {}
    PhysxSuite& runtime;
    PhysxStats published;
    PhysxBodyFrame frameOut;
    std::vector<float> poses, previous;      // drawn layout: grouped by look
    std::vector<float> lastPose;             // per body, its pose last frame (8 floats)
    bool havePrevious = false;

#if defined(BOO_HAS_PHYSX)
    physx::PxScene* scene = nullptr;
    physx::PxMaterial* material = nullptr;
    std::vector<physx::PxRigidActor*> statics;
    std::vector<physx::PxRigidDynamic*> bodies;
    std::vector<float> radius;
    uint32_t act = UINT32_MAX;
    bool simulating = false;
    PhysxStepper stepper{1.0 / 60.0, 3u};
    double spawnOwed = 0.0;
    double sinceShower = 0.0; // seconds since the last shower finished falling
    uint32_t nextRecycle = 0;
    uint32_t seed = 0x9E3779B9u;
    float groundY = 0.0f;
    bool gpu = false;

    float rand01() {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return float(seed & 0xFFFFFFu) / 16777215.0f;
    }
    void releaseScene();
    bool buildScene(const std::vector<CityCollisionBox>& buildings);
    void place(physx::PxRigidDynamic* body, float r, const float air[3]);
#endif
};

#if defined(BOO_HAS_PHYSX)
using namespace physx;

void PhysxRigidGenre::Impl::releaseScene() {
    if (!scene) return;
    if (simulating) scene->fetchResults(true);
    simulating = false;
    for (auto* b : bodies) { scene->removeActor(*b); b->release(); }
    for (auto* s : statics) { scene->removeActor(*s); s->release(); }
    bodies.clear();
    statics.clear();
    radius.clear();
    lastPose.clear();
    scene->release();
    scene = nullptr;
    if (material) material->release();
    material = nullptr;
    havePrevious = false;
    act = UINT32_MAX;
}

bool PhysxRigidGenre::Impl::buildScene(const std::vector<CityCollisionBox>& buildings) {
    auto* physics = static_cast<PxPhysics*>(runtime.physics());
    auto* dispatcher = static_cast<PxCpuDispatcher*>(runtime.dispatcher());
    auto* cuda = static_cast<PxCudaContextManager*>(runtime.cudaContextManager());
    if (!physics || !dispatcher) return false;
    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    desc.flags |= PxSceneFlag::eENABLE_PCM;
    gpu = cuda != nullptr;
    if (gpu) {
        desc.cudaContextManager = cuda;
        desc.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS;
        desc.broadPhaseType = PxBroadPhaseType::eGPU;
        desc.gpuDynamicsConfig.maxRigidPatchCount = 1u << 18;
        desc.gpuDynamicsConfig.maxRigidContactCount = 1u << 20;
        desc.gpuMaxNumPartitions = 8;
    }
    desc.solverType = PxSolverType::eTGS;
    scene = physics->createScene(desc);
    if (!scene) return false;
    material = physics->createMaterial(0.45f, 0.35f, 0.35f); // glazed enamel on stone

    // The buildings, and the ground just under the lowest of them.
    groundY = 1e9f;
    for (const auto& b : buildings) {
        const PxTransform pose(PxVec3(b.center[0], b.center[1], b.center[2]),
                               PxQuat(b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3]).getNormalized());
        PxRigidStatic* actor = PxCreateStatic(*physics, pose, PxBoxGeometry(b.half[0], b.half[1], b.half[2]), *material);
        if (!actor) continue;
        scene->addActor(*actor);
        statics.push_back(actor);
        groundY = std::min(groundY, b.center[1] - b.half[1]);
    }
    if (groundY > 1e8f) groundY = 0.0f;
    PxRigidStatic* ground = PxCreatePlane(*physics, PxPlane(0.0f, 1.0f, 0.0f, -groundY), *material);
    scene->addActor(*ground);
    statics.push_back(ground);
    spawnOwed = 0.0;
    nextRecycle = 0;
    return true;
}

void PhysxRigidGenre::Impl::place(PxRigidDynamic* body, float r, const float air[3]) {
    // Over the birds' airspace, high above it, scattered across a disc thirty metres wide.
    const float angle = 6.2831853f * rand01();
    const float out = 7.0f * std::sqrt(rand01()); // a block of the city, not all of it
    const PxVec3 at(air[0] + out * std::cos(angle), air[1] + 55.0f + 10.0f * rand01(), air[2] + out * std::sin(angle));
    const PxQuat turn(6.2831853f * rand01(), PxVec3(rand01() - 0.5f, rand01() - 0.5f, rand01() - 0.5f).getNormalized());
    body->setGlobalPose(PxTransform(at, turn));
    body->setLinearVelocity(PxVec3(1.5f * (rand01() - 0.5f), -4.0f, 1.5f * (rand01() - 0.5f)));
    body->setAngularVelocity(PxVec3(rand01() - 0.5f, rand01() - 0.5f, rand01() - 0.5f) * 4.0f);
    (void)r;
}
#endif

PhysxRigidGenre::PhysxRigidGenre(PhysxSuite& runtime) : impl_(std::make_unique<Impl>(runtime)) {
    impl_->published.genre = static_cast<uint32_t>(PhysxGenre::Rigid);
    impl_->published.state = 3u; // starting until the scene exists
}

PhysxRigidGenre::~PhysxRigidGenre() {
#if defined(BOO_HAS_PHYSX)
    impl_->releaseScene();
#endif
}

const PhysxBodyFrame* PhysxRigidGenre::bodies() const noexcept {
    return impl_->frameOut.count ? &impl_->frameOut : nullptr;
}

PhysxStats PhysxRigidGenre::stats() const { return impl_->published; }

void PhysxRigidGenre::frame(double frameSeconds, bool paused, const std::vector<CityCollisionBox>& buildings,
                            uint32_t buildingsAct, const float air[3], bool haveAir) {
    Impl& m = *impl_;
#if !defined(BOO_HAS_PHYSX)
    (void)frameSeconds; (void)paused; (void)buildings; (void)buildingsAct; (void)air; (void)haveAir;
    m.published.state = 0u;
    m.published.reason = "This build does not include the NVIDIA PhysX SDK";
#else
    const PhysxRuntimeInfo runtime = m.runtime.info();
    if (runtime.state != PhysxRuntimeState::Ready) {
        m.published.state = runtime.state == PhysxRuntimeState::Starting ? 3u : 0u;
        m.published.reason = runtime.reason;
        m.frameOut.count = 0;
        return;
    }
    // A city to land on: rebuild when the film cuts to the other city.
    if (buildingsAct == UINT32_MAX || buildings.empty() || !haveAir) {
        if (!m.scene) { m.published.state = 3u; m.frameOut.count = 0; }
        if (m.scene && buildingsAct != m.act) { m.releaseScene(); m.frameOut.count = 0; }
        return;
    }
    if (m.scene && buildingsAct != m.act) m.releaseScene();
    if (!m.scene) {
        const auto began = std::chrono::steady_clock::now();
        if (!m.buildScene(buildings)) {
            m.published.state = 0u;
            m.published.reason = "PhysX scene creation failed";
            return;
        }
        m.act = buildingsAct;
        m.published.backend = std::string("NVIDIA PhysX ") + runtime.sdkVersion +
                              (m.gpu ? ", GPU rigid bodies on " + runtime.cudaDevice : ", CPU rigid bodies");
        BOO_LOG_INFO("PhysX", "Rigid bodies: " + std::to_string(m.statics.size() - 1) + " buildings as static boxes, built in " +
                              std::to_string(static_cast<int>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count())) +
                              " ms (" + m.published.backend + ").");
    }
    m.published.state = 2u;
    m.published.reason.clear();

    const auto t0 = std::chrono::steady_clock::now();
    // Results of the step started last frame.
    if (m.simulating) {
        m.scene->fetchResults(true);
        m.simulating = false;
    }
    const auto tFetched = std::chrono::steady_clock::now();
    PxSimulationStatistics statistics;
    m.scene->getSimulationStatistics(statistics); // only allowed while no step is running

    // Pour: up to the cap, then recycle bodies that have settled or left the city.
    auto* physics = static_cast<PxPhysics*>(m.runtime.physics());
    const uint32_t steps = m.stepper.advance(frameSeconds, paused, false);
    if (!paused) {
        m.spawnOwed += std::min(frameSeconds, 0.1) * 700.0; // the first shower falls over about four seconds
        while (m.spawnOwed >= 1.0 && m.bodies.size() < kMaxBodies) {
            m.spawnOwed -= 1.0;
            const float r = 0.30f + 0.30f * m.rand01();
            PxRigidDynamic* body = PxCreateDynamic(*physics, PxTransform(PxIdentity), PxSphereGeometry(r), *m.material, 2400.0f);
            if (!body) break;
            body->setSleepThreshold(0.05f);
            m.place(body, r, air);
            m.scene->addActor(*body);
            m.bodies.push_back(body);
            m.radius.push_back(r);
            m.lastPose.resize(m.bodies.size() * 8u, 0.0f);
        }
        if (m.bodies.size() >= kMaxBodies) {
            // The shower lands and rests; after half a minute the resting marbles rise for the next one.
            m.sinceShower += std::min(frameSeconds, 0.1);
            const bool nextShower = m.sinceShower > 30.0;
            uint32_t budget = static_cast<uint32_t>(std::min(frameSeconds, 0.1) * 700.0) + 1u;
            uint32_t lifted = 0;
            for (uint32_t tries = 0; tries < 1024u && budget; ++tries) {
                PxRigidDynamic* body = m.bodies[m.nextRecycle];
                const uint32_t idx = m.nextRecycle;
                m.nextRecycle = (m.nextRecycle + 1u) % static_cast<uint32_t>(m.bodies.size());
                const float y = body->getGlobalPose().p.y;
                if ((nextShower && body->isSleeping()) || y < m.groundY - 5.0f) {
                    ++lifted;
                    m.place(body, m.radius[idx], air);
                    body->wakeUp();
                    --budget;
                }
            }
            if (nextShower && lifted == 0) m.sinceShower = 0.0; // everyone is back in the air
        }
    }

    // Read every body (results are a frame behind the step that is about to start).
    const uint32_t n = static_cast<uint32_t>(m.bodies.size());
    m.poses.resize(size_t(n) * 8u);
    m.previous.resize(size_t(n) * 8u);
    uint32_t start[7]{};
    for (uint32_t look = 0; look < 6u; ++look) start[look + 1] = start[look] + n / 6u + (look < n % 6u ? 1u : 0u);
    uint32_t cursor[6];
    for (uint32_t look = 0; look < 6u; ++look) cursor[look] = start[look];
    for (uint32_t i = 0; i < n; ++i) {
        const PxTransform pose = m.bodies[i]->getGlobalPose();
        float* out = m.poses.data() + size_t(cursor[i % 6u]) * 8u;
        float* before = m.previous.data() + size_t(cursor[i % 6u]) * 8u;
        ++cursor[i % 6u];
        const float now[8] = {pose.p.x, pose.p.y, pose.p.z, pose.q.x, pose.q.y, pose.q.z, pose.q.w, m.radius[i]};
        float* last = m.lastPose.data() + size_t(i) * 8u;
        const bool known = last[7] > 0.0f;
        std::copy(now, now + 8, out);
        if (known) std::copy(last, last + 8, before); else std::copy(now, now + 8, before);
        std::copy(now, now + 8, last);
    }
    m.frameOut.count = n;
    m.frameOut.poses = m.poses.data();
    m.frameOut.previousPoses = m.previous.data();
    std::copy(start, start + 7, m.frameOut.lookStart);

    // Advance: all but the last step now, the last one in the background until next frame.
    for (uint32_t s = 0; s + 1 < steps; ++s) {
        m.scene->simulate(static_cast<PxReal>(m.stepper.stepSeconds()));
        m.scene->fetchResults(true);
    }
    if (steps > 0) {
        m.scene->simulate(static_cast<PxReal>(m.stepper.stepSeconds()));
        m.simulating = true;
    }
    const auto t1 = std::chrono::steady_clock::now();

    auto smooth = [](float& v, float sample) { v = v > 0.0f ? v * 0.9f + sample * 0.1f : sample; };
    smooth(m.published.stepWallMs, std::chrono::duration<float, std::milli>(tFetched - t0).count() +
                                       (steps > 1 ? std::chrono::duration<float, std::milli>(t1 - tFetched).count() : 0.0f));
    smooth(m.published.stepCpuMs, std::chrono::duration<float, std::milli>(t1 - tFetched).count());
    m.published.counters[0] = static_cast<float>(n);
    m.published.counters[1] = static_cast<float>(statistics.nbActiveDynamicBodies);
    m.published.counters[2] = static_cast<float>(statistics.nbDiscreteContactPairsTotal);
    m.published.counters[3] = static_cast<float>(m.statics.size() - 1);
    m.published.counterNames[0] = "bodies";
    m.published.counterNames[1] = "awake";
    m.published.counterNames[2] = "contact_pairs";
    m.published.counterNames[3] = "buildings";
    m.published.gpuMemoryMegabytes = static_cast<float>(double(statistics.gpuMemHeap) / (1024.0 * 1024.0));
    m.published.steps = m.stepper.totalSteps();
#endif
}

} // namespace boo
