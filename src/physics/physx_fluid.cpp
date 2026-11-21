#include "boo/physx_fluid.hpp"

#include "boo/logging.hpp"
#include "boo/physx_genres.hpp"
#include "boo/physx_stepper.hpp"
#include "boo/wind_field.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#if defined(BOO_HAS_PHYSX)
#pragma warning(push, 0)
#include "PxPhysicsAPI.h"
#include "extensions/PxParticleExt.h"
#include "extensions/PxCudaHelpersExt.h"
#include "cudamanager/PxCudaContext.h"
#pragma warning(pop)
#endif

namespace boo {

namespace {
constexpr float kSpacing = 0.30f;             // metres between particles at rest
constexpr float kDrawRadius = 0.17f;          // the glass bead each particle is drawn as
} // namespace

struct PhysxFluidGenre::Impl {
    explicit Impl(PhysxSuite& r) : runtime(r) {}
    PhysxSuite& runtime;
    PhysxStats published;
    PhysxBodyFrame frameOut;
    std::vector<float> poses, previous;

#if defined(BOO_HAS_PHYSX)
    physx::PxScene* scene = nullptr;
    physx::PxMaterial* material = nullptr;
    physx::PxPBDMaterial* water = nullptr;
    physx::PxPBDParticleSystem* system = nullptr;
    physx::PxParticleBuffer* buffer = nullptr;
    std::vector<physx::PxRigidActor*> statics;
    std::vector<physx::PxVec4> host, hostVelocity;
    uint32_t act = UINT32_MAX;
    bool simulating = false;
    PhysxStepper stepper{1.0 / 60.0, 2u};
    float groundY = 0.0f;
    uint32_t seed = 0x2545F491u;
    float rand01() {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return float(seed & 0xFFFFFFu) / 16777215.0f;
    }
    physx::PxVec4 rainDrop(const float air[3]) {
        const float angle = 6.2831853f * rand01();
        const float out = 6.0f * std::sqrt(rand01()); // one block, not the city
        return {air[0] + out * std::cos(angle), air[1] + 50.0f + 25.0f * rand01(), air[2] + out * std::sin(angle), 1.0f / 0.02f};
    }
    void releaseScene();
    bool buildScene(const std::vector<CityCollisionBox>& buildings, const float air[3]);
#endif
};

#if defined(BOO_HAS_PHYSX)
using namespace physx;

void PhysxFluidGenre::Impl::releaseScene() {
    if (!scene) return;
    if (simulating) scene->fetchResults(true);
    simulating = false;
    if (system) {
        if (buffer) { system->removeParticleBuffer(buffer); buffer->release(); }
        scene->removeActor(*system);
        system->release();
    }
    buffer = nullptr;
    system = nullptr;
    for (auto* s : statics) { scene->removeActor(*s); s->release(); }
    statics.clear();
    scene->release();
    scene = nullptr;
    if (water) water->release();
    if (material) material->release();
    water = nullptr;
    material = nullptr;
    act = UINT32_MAX;
}

bool PhysxFluidGenre::Impl::buildScene(const std::vector<CityCollisionBox>& buildings, const float air[3]) {
    auto* physics = static_cast<PxPhysics*>(runtime.physics());
    auto* dispatcher = static_cast<PxCpuDispatcher*>(runtime.dispatcher());
    auto* cuda = static_cast<PxCudaContextManager*>(runtime.cudaContextManager());
    if (!physics || !dispatcher || !cuda) return false; // PhysX particles run on the GPU only
    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    desc.cudaContextManager = cuda;
    desc.flags |= PxSceneFlag::eENABLE_PCM | PxSceneFlag::eENABLE_GPU_DYNAMICS;
    desc.broadPhaseType = PxBroadPhaseType::eGPU;
    desc.solverType = PxSolverType::eTGS;
    desc.gpuDynamicsConfig.maxRigidPatchCount = 1u << 16;
    scene = physics->createScene(desc);
    if (!scene) return false;
    material = physics->createMaterial(0.3f, 0.3f, 0.0f);
    groundY = 1e9f;
    for (const auto& b : buildings) {
        const PxTransform pose(PxVec3(b.center[0], b.center[1], b.center[2]),
                               PxQuat(b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3]).getNormalized());
        if (PxRigidStatic* actor = PxCreateStatic(*physics, pose, PxBoxGeometry(b.half[0], b.half[1], b.half[2]), *material)) {
            scene->addActor(*actor);
            statics.push_back(actor);
            groundY = std::min(groundY, b.center[1] - b.half[1]);
        }
    }
    if (groundY > 1e8f) groundY = 0.0f;
    PxRigidStatic* ground = PxCreatePlane(*physics, PxPlane(0.0f, 1.0f, 0.0f, -groundY), *material);
    scene->addActor(*ground);
    statics.push_back(ground);

    // Water: low viscosity, a little surface tension and cohesion, as in NVIDIA's PBF sample.
    water = physics->createPBDMaterial(0.05f, 0.05f, 0.0f, 0.001f, 0.5f, 0.005f, 0.01f, 0.0f, 0.0f);
    water->setVorticityConfinement(10.0f);
    system = physics->createPBDParticleSystem(*cuda, 96);
    const float restOffset = 0.5f * kSpacing / 0.6f;
    system->setRestOffset(restOffset);
    system->setContactOffset(restOffset + 0.02f);
    system->setParticleContactOffset(restOffset);
    system->setSolidRestOffset(restOffset);
    system->setFluidRestOffset(restOffset * 0.6f);
    system->setMaxVelocity(30.0f);
    scene->addActor(*system);
    const PxU32 phase = system->createPhase(water, PxParticlePhaseFlags(PxParticlePhaseFlag::eParticlePhaseFluid |
                                                                        PxParticlePhaseFlag::eParticlePhaseSelfCollide));
    PxU32* phases = PX_EXT_PINNED_MEMORY_ALLOC(PxU32, *cuda, kMaxParticles);
    PxVec4* positions = PX_EXT_PINNED_MEMORY_ALLOC(PxVec4, *cuda, kMaxParticles);
    PxVec4* velocities = PX_EXT_PINNED_MEMORY_ALLOC(PxVec4, *cuda, kMaxParticles);
    for (PxU32 i = 0; i < kMaxParticles; ++i) {
        phases[i] = phase;
        positions[i] = rainDrop(air);
        velocities[i] = PxVec4(0.0f, -6.0f, 0.0f, 0.0f);
    }
    ExtGpu::PxParticleBufferDesc bufferDesc;
    bufferDesc.maxParticles = kMaxParticles;
    bufferDesc.numActiveParticles = kMaxParticles;
    bufferDesc.positions = positions;
    bufferDesc.velocities = velocities;
    bufferDesc.phases = phases;
    buffer = ExtGpu::PxCreateAndPopulateParticleBuffer(bufferDesc, cuda);
    PX_EXT_PINNED_MEMORY_FREE(*cuda, positions);
    PX_EXT_PINNED_MEMORY_FREE(*cuda, velocities);
    PX_EXT_PINNED_MEMORY_FREE(*cuda, phases);
    if (!buffer) return false;
    system->addParticleBuffer(buffer);
    host.assign(kMaxParticles, PxVec4(0.0f));
    hostVelocity.assign(kMaxParticles, PxVec4(0.0f));
    return true;
}
#endif

PhysxFluidGenre::PhysxFluidGenre(PhysxSuite& runtime) : impl_(std::make_unique<Impl>(runtime)) {
    impl_->published.genre = static_cast<uint32_t>(PhysxGenre::Fluid);
    impl_->published.state = 3u;
}

PhysxFluidGenre::~PhysxFluidGenre() {
#if defined(BOO_HAS_PHYSX)
    impl_->releaseScene();
#endif
}

const PhysxBodyFrame* PhysxFluidGenre::bodies() const noexcept { return impl_->frameOut.count ? &impl_->frameOut : nullptr; }
PhysxStats PhysxFluidGenre::stats() const { return impl_->published; }

void PhysxFluidGenre::frame(double frameSeconds, double filmSeconds, bool paused, const std::vector<CityCollisionBox>& buildings,
                            uint32_t buildingsAct, const float air[3], bool haveAir) {
    Impl& m = *impl_;
#if !defined(BOO_HAS_PHYSX)
    (void)frameSeconds; (void)filmSeconds; (void)paused; (void)buildings; (void)buildingsAct; (void)air; (void)haveAir;
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
    if (!runtime.gpu) {
        m.published.state = 0u;
        m.published.reason = "PhysX fluids need the GPU solvers (CUDA)";
        m.frameOut.count = 0;
        return;
    }
    if (buildingsAct == UINT32_MAX || buildings.empty() || !haveAir) {
        if (m.scene && buildingsAct != m.act) m.releaseScene();
        if (!m.scene) { m.published.state = 3u; m.frameOut.count = 0; }
        return;
    }
    if (m.scene && buildingsAct != m.act) m.releaseScene();
    if (!m.scene) {
        if (!m.buildScene(buildings, air)) {
            m.releaseScene();
            m.published.state = 0u;
            m.published.reason = "PhysX particle scene creation failed";
            return;
        }
        m.act = buildingsAct;
        m.published.backend = "NVIDIA PhysX " + runtime.sdkVersion + ", GPU position-based fluid on " + runtime.cudaDevice;
        BOO_LOG_INFO("PhysX", "Fluids: " + std::to_string(kMaxParticles) + " water particles over " + std::to_string(m.statics.size() - 1) +
                              " buildings (" + m.published.backend + ").");
    }
    m.published.state = 2u;
    m.published.reason.clear();

    const auto t0 = std::chrono::steady_clock::now();
    if (m.simulating) { m.scene->fetchResults(true); m.simulating = false; }
    const auto tFetched = std::chrono::steady_clock::now();
    PxSimulationStatistics statistics;
    m.scene->getSimulationStatistics(statistics);

    // Read the water back from the GPU.
    auto* cuda = static_cast<PxCudaContextManager*>(m.runtime.cudaContextManager());
    {
        PxScopedCudaLock lock(*cuda);
        cuda->getCudaContext()->memcpyDtoH(m.host.data(), reinterpret_cast<CUdeviceptr>(m.buffer->getPositionInvMasses()),
                                           sizeof(PxVec4) * kMaxParticles);
    }
    // Water that has pooled in the streets, or run out of the city, falls again as fresh rain.
    bool respawned = false;
    if (!paused) {
        uint32_t budget = static_cast<uint32_t>(std::min(frameSeconds, 0.1) * 1200.0) + 1u;
        for (uint32_t i = 0; i < kMaxParticles && budget; ++i) {
            const PxVec4& p = m.host[i];
            const float dx = p.x - air[0], dz = p.z - air[2];
            if (p.y < m.groundY + 0.8f || dx * dx + dz * dz > 160.0f * 160.0f || !std::isfinite(p.y)) {
                m.host[i] = m.rainDrop(air);
                m.hostVelocity[i] = PxVec4(0.0f, -6.0f, 0.0f, 0.0f);
                respawned = true;
                --budget;
            }
        }
    }
    if (respawned) {
        // Velocities of untouched particles are left alone: only positions and the reset drops are written.
        PxScopedCudaLock lock(*cuda);
        cuda->getCudaContext()->memcpyHtoD(reinterpret_cast<CUdeviceptr>(m.buffer->getPositionInvMasses()), m.host.data(),
                                           sizeof(PxVec4) * kMaxParticles);
        m.buffer->raiseFlags(PxParticleBufferFlag::eUPDATE_POSITION);
    }
    const float windAtAir[3] = {air[0], air[1] + 30.0f, air[2]};
    const auto wind = windAt(windAtAir, static_cast<float>(filmSeconds));
    m.system->setWind(PxVec3(wind[0], wind[1], wind[2]));

    // Draw: every particle a glass bead (look 0 is the crystal marble).
    m.poses.resize(size_t(kMaxParticles) * 8u);
    const bool hadPrevious = !m.previous.empty();
    if (hadPrevious) m.previous.swap(m.poses);
    m.poses.resize(size_t(kMaxParticles) * 8u);
    for (uint32_t i = 0; i < kMaxParticles; ++i) {
        float* out = m.poses.data() + size_t(i) * 8u;
        out[0] = m.host[i].x; out[1] = m.host[i].y; out[2] = m.host[i].z;
        out[3] = 0.0f; out[4] = 0.0f; out[5] = 0.0f; out[6] = 1.0f; out[7] = kDrawRadius;
    }
    if (!hadPrevious) m.previous = m.poses;
    m.frameOut.count = kMaxParticles;
    m.frameOut.poses = m.poses.data();
    m.frameOut.previousPoses = m.previous.data();
    for (uint32_t k = 0; k < 7u; ++k) m.frameOut.lookStart[k] = k == 0 ? 0u : kMaxParticles;

    const uint32_t steps = m.stepper.advance(frameSeconds, paused, false);
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
    smooth(m.published.stepWallMs, std::chrono::duration<float, std::milli>(tFetched - t0).count());
    smooth(m.published.stepCpuMs, std::chrono::duration<float, std::milli>(t1 - tFetched).count());
    m.published.counters[0] = static_cast<float>(kMaxParticles);
    m.published.counters[1] = static_cast<float>(m.statics.size() - 1);
    m.published.counterNames[0] = "particles";
    m.published.counterNames[1] = "buildings";
    m.published.gpuMemoryMegabytes = static_cast<float>(double(statistics.gpuMemParticles + statistics.gpuMemHeap) / (1024.0 * 1024.0));
    m.published.steps = m.stepper.totalSteps();
#endif
}

} // namespace boo
