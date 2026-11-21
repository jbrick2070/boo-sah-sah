#include "boo/physx_soft.hpp"

#include "boo/logging.hpp"
#include "boo/physx_genres.hpp"
#include "boo/physx_stepper.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <map>

#if defined(BOO_HAS_PHYSX)
#pragma warning(push, 0)
#include "PxPhysicsAPI.h"
#include "extensions/PxDeformableVolumeExt.h"
#include "extensions/PxCudaHelpersExt.h"
#include "cudamanager/PxCudaContext.h"
#pragma warning(pop)
#endif

namespace boo {

struct PhysxSoftGenre::Impl {
    explicit Impl(PhysxSuite& r) : runtime(r) {}
    PhysxSuite& runtime;
    PhysxStats published;
    PhysxBodyFrame frameOut;
    std::vector<float> poses, previous, stretch, previousStretch;
    bool havePrevious = false;

#if defined(BOO_HAS_PHYSX)
    struct Microbe {
        physx::PxDeformableVolume* body = nullptr;
        float radius = 0.5f;
        physx::PxVec4* simPos = nullptr;   // pinned host mirrors
        physx::PxVec4* simVel = nullptr;
        physx::PxVec4* collPos = nullptr;
        physx::PxVec4* restPos = nullptr;
        physx::PxDeformableVolumeMesh* mesh = nullptr; // its own: PhysX does not share a mesh's auxiliary data
        bool resting = false;
    };
    physx::PxScene* scene = nullptr;
    physx::PxMaterial* stone = nullptr;
    physx::PxDeformableVolumeMaterial* jelly = nullptr;
    physx::PxDeformableVolumeMesh* mesh = nullptr; // one unit-radius microbe, shared by all
    std::vector<physx::PxVec4> unitSim, unitColl, unitRest; // the mesh's own buffers, before placing
    std::vector<physx::PxRigidActor*> statics;
    std::vector<Microbe> microbes;
    std::vector<physx::PxVec4> readback;
    uint32_t act = UINT32_MAX;
    uint32_t lost = 0; // bodies re-dropped after the solver lost them
    uint32_t stepsLast = 0; // steps started last frame: what this frame's readback reflects
    bool simulating = false;
    PhysxStepper stepper{1.0 / 60.0, 3u};
    double sinceSettled = 0.0;
    uint32_t seed = 0x2545F491u;
    float groundY = 0.0f;

    float rand01() {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return float(seed & 0xFFFFFFu) / 16777215.0f;
    }
    void releaseScene();
    bool buildScene(const std::vector<CityCollisionBox>& buildings);
    void place(Microbe& m, const float air[3], uint32_t slot);
#endif
};

#if defined(BOO_HAS_PHYSX)
using namespace physx;

namespace {
// A unit icosphere, subdivided twice: 162 vertices, 320 triangles.
void unitSphere(std::vector<PxVec3>& verts, std::vector<PxU32>& tris) {
    const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
    verts = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    tris = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
            3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    for (auto& v : verts) v.normalize();
    for (int level = 0; level < 2; ++level) {
        std::map<std::pair<PxU32, PxU32>, PxU32> mid;
        auto middle = [&](PxU32 a, PxU32 b) {
            const auto key = std::minmax(a, b);
            if (auto it = mid.find(key); it != mid.end()) return it->second;
            verts.push_back(((verts[a] + verts[b]) * 0.5f).getNormalized());
            return mid[key] = PxU32(verts.size() - 1);
        };
        std::vector<PxU32> next;
        for (size_t i = 0; i < tris.size(); i += 3) {
            const PxU32 a = tris[i], b = tris[i + 1], c = tris[i + 2], ab = middle(a, b), bc = middle(b, c), ca = middle(c, a);
            next.insert(next.end(), {a, ab, ca, b, bc, ab, c, ca, bc, ab, bc, ca});
        }
        tris.swap(next);
    }
}

// Eigen-decomposition of a symmetric 3x3 matrix by Jacobi rotations: columns of `vectors` are the axes.
void symmetricEigen(float a[3][3], float vectors[3][3]) {
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) vectors[i][j] = i == j ? 1.0f : 0.0f;
    for (int sweep = 0; sweep < 12; ++sweep) {
        for (int p = 0; p < 2; ++p)
            for (int q = p + 1; q < 3; ++q) {
                if (std::fabs(a[p][q]) < 1e-12f) continue;
                const float theta = 0.5f * std::atan2(2.0f * a[p][q], a[q][q] - a[p][p]);
                const float c = std::cos(theta), s = std::sin(theta);
                for (int k = 0; k < 3; ++k) { // a = J^T a J
                    const float akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq; a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const float apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk; a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const float vkp = vectors[k][p], vkq = vectors[k][q];
                    vectors[k][p] = c * vkp - s * vkq; vectors[k][q] = s * vkp + c * vkq;
                }
            }
    }
}
} // namespace

void PhysxSoftGenre::Impl::releaseScene() {
    if (!scene) return;
    auto* cuda = static_cast<PxCudaContextManager*>(runtime.cudaContextManager());
    if (simulating) scene->fetchResults(true);
    simulating = false;
    for (auto& m : microbes) {
        scene->removeActor(*m.body);
        m.body->release();
        if (m.mesh) m.mesh->release();
        PX_EXT_PINNED_MEMORY_FREE(*cuda, m.simPos);
        PX_EXT_PINNED_MEMORY_FREE(*cuda, m.simVel);
        PX_EXT_PINNED_MEMORY_FREE(*cuda, m.collPos);
        PX_EXT_PINNED_MEMORY_FREE(*cuda, m.restPos);
    }
    microbes.clear();
    for (auto* s : statics) { scene->removeActor(*s); s->release(); }
    statics.clear();
    scene->release();
    scene = nullptr;
    if (mesh) mesh->release();
    mesh = nullptr;
    if (jelly) jelly->release();
    jelly = nullptr;
    if (stone) stone->release();
    stone = nullptr;
    havePrevious = false;
    act = UINT32_MAX;
}

void PhysxSoftGenre::Impl::place(Microbe& m, const float air[3], uint32_t slot) {
    // Copy the unit microbe back into the host mirrors, then size, turn and drop it over the block.
    std::copy(unitSim.begin(), unitSim.end(), m.simPos);
    std::copy(unitColl.begin(), unitColl.end(), m.collPos);
    std::copy(unitRest.begin(), unitRest.end(), m.restPos);
    for (size_t i = 0; i < unitSim.size(); ++i) m.simVel[i] = PxVec4(0.0f);
    // Rings of eight, three metres apart in height, so no two microbes start inside each other (soft
    // bodies that begin overlapping are thrown apart).
    const uint32_t ring = slot / 8u, place = slot % 8u;
    const float angle = 6.2831853f * (float(place) + 0.5f * float(ring & 1u)) / 8.0f + 0.2f * (rand01() - 0.5f);
    const float out = 4.0f + 0.6f * float(ring % 3u);
    const PxVec3 at(air[0] + out * std::cos(angle), air[1] + 16.0f + 3.0f * float(ring), air[2] + out * std::sin(angle));
    const PxQuat turn(6.2831853f * rand01(), PxVec3(rand01() - 0.5f, rand01() - 0.5f, rand01() - 0.5f).getNormalized());
    PxDeformableVolumeExt::transform(*m.body, PxTransform(at, turn), m.radius, m.simPos, m.simVel, m.collPos, m.restPos);
    PxDeformableVolumeExt::updateMass(*m.body, 1100.0f, 50.0f, m.simPos); // a little denser than water
    const PxVec4 fall(0.6f * (rand01() - 0.5f), -5.0f, 0.6f * (rand01() - 0.5f), 0.0f);
    for (size_t i = 0; i < unitSim.size(); ++i) m.simVel[i] = fall;
    PxDeformableVolumeExt::copyToDevice(*m.body, PxDeformableVolumeDataFlag::eALL, m.simPos, m.simVel, m.collPos, m.restPos);
    m.resting = false;
}

bool PhysxSoftGenre::Impl::buildScene(const std::vector<CityCollisionBox>& buildings) {
    auto* physics = static_cast<PxPhysics*>(runtime.physics());
    auto* dispatcher = static_cast<PxCpuDispatcher*>(runtime.dispatcher());
    auto* cuda = static_cast<PxCudaContextManager*>(runtime.cudaContextManager());
    if (!physics || !dispatcher || !cuda) return false; // deformable volumes run only on the GPU
    PxSceneDesc desc(physics->getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    desc.cudaContextManager = cuda;
    desc.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS | PxSceneFlag::eENABLE_PCM;
    desc.broadPhaseType = PxBroadPhaseType::eGPU;
    desc.solverType = PxSolverType::eTGS;
    scene = physics->createScene(desc);
    if (!scene) return false;
    stone = physics->createMaterial(0.6f, 0.5f, 0.1f);
    // Soft, wet jelly: Young's modulus 30 kPa, nearly incompressible, a little damping.
    jelly = physics->createDeformableVolumeMaterial(6.0e4f, 0.45f, 0.6f, 0.05f);

    groundY = 1e9f;
    for (const auto& b : buildings) {
        const PxTransform pose(PxVec3(b.center[0], b.center[1], b.center[2]), PxQuat(b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3]).getNormalized());
        if (PxRigidStatic* actor = PxCreateStatic(*physics, pose, PxBoxGeometry(b.half[0], b.half[1], b.half[2]), *stone)) {
            scene->addActor(*actor);
            statics.push_back(actor);
            groundY = std::min(groundY, b.center[1] - b.half[1]);
        }
    }
    if (groundY > 1e8f) groundY = 0.0f;
    PxRigidStatic* ground = PxCreatePlane(*physics, PxPlane(0.0f, 1.0f, 0.0f, -groundY), *stone);
    scene->addActor(*ground);
    statics.push_back(ground);

    // One unit microbe, cooked once: its surface, then a conforming collision mesh and a voxel
    // simulation mesh.
    std::vector<PxVec3> verts;
    std::vector<PxU32> tris;
    unitSphere(verts, tris);
    PxCookingParams cooking(physics->getTolerancesScale());
    cooking.meshWeldTolerance = 0.001f;
    cooking.meshPreprocessParams = PxMeshPreprocessingFlags(PxMeshPreprocessingFlag::eWELD_VERTICES);
    cooking.buildTriangleAdjacencies = false;
    cooking.buildGPUData = true;
    PxSimpleTriangleMesh surface;
    surface.points.count = PxU32(verts.size());
    surface.points.stride = sizeof(PxVec3);
    surface.points.data = verts.data();
    surface.triangles.count = PxU32(tris.size() / 3);
    surface.triangles.stride = 3 * sizeof(PxU32);
    surface.triangles.data = tris.data();
    mesh = PxDeformableVolumeExt::createDeformableVolumeMesh(cooking, surface, 5, physics->getPhysicsInsertionCallback());
    if (!mesh) return false;

    for (uint32_t i = 0; i < kMicrobes; ++i) {
        Microbe m;
        m.radius = 0.45f + 0.35f * rand01();
        m.mesh = i == 0 ? mesh : PxDeformableVolumeExt::createDeformableVolumeMesh(cooking, surface, 5, physics->getPhysicsInsertionCallback());
        if (i == 0) mesh->acquireReference();
        m.body = physics->createDeformableVolume(*cuda);
        if (!m.mesh || !m.body) return false;
        PxDeformableVolumeMaterial* materials[] = {jelly};
        PxShape* shape = physics->createShape(PxTetrahedronMeshGeometry(m.mesh->getCollisionMesh()), reinterpret_cast<PxDeformableVolumeMaterial**>(materials), 1, true,
                                              PxShapeFlag::eSIMULATION_SHAPE);
        m.body->attachShape(*shape);
        shape->release();
        m.body->attachSimulationMesh(*m.mesh->getSimulationMesh(), *m.mesh->getDeformableVolumeAuxData());
        scene->addActor(*m.body);
        PxDeformableVolumeExt::allocateAndInitializeHostMirror(*m.body, cuda, m.simPos, m.simVel, m.collPos, m.restPos);
        if (unitSim.empty()) {
            unitSim.assign(m.simPos, m.simPos + mesh->getSimulationMesh()->getNbVertices());
            unitColl.assign(m.collPos, m.collPos + mesh->getCollisionMesh()->getNbVertices());
            unitRest.assign(m.restPos, m.restPos + mesh->getCollisionMesh()->getNbVertices());
        }
        m.body->setDeformableBodyFlag(PxDeformableBodyFlag::eDISABLE_SELF_COLLISION, true);
        m.body->setSolverIterationCounts(8);
        m.body->setMaxDepenetrationVelocity(3.0f);
        m.body->setMaxLinearVelocity(20.0f);
        m.body->setLinearDamping(0.05f);
        microbes.push_back(m);
    }
    return true;
}
#endif

PhysxSoftGenre::PhysxSoftGenre(PhysxSuite& runtime) : impl_(std::make_unique<Impl>(runtime)) {
    impl_->published.genre = static_cast<uint32_t>(PhysxGenre::Soft);
    impl_->published.state = 3u;
}

PhysxSoftGenre::~PhysxSoftGenre() {
#if defined(BOO_HAS_PHYSX)
    impl_->releaseScene();
#endif
}

const PhysxBodyFrame* PhysxSoftGenre::bodies() const noexcept { return impl_->frameOut.count ? &impl_->frameOut : nullptr; }

PhysxStats PhysxSoftGenre::stats() const { return impl_->published; }

void PhysxSoftGenre::frame(double frameSeconds, bool paused, const std::vector<CityCollisionBox>& buildings, uint32_t buildingsAct,
                           const float air[3], bool haveAir) {
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
    if (!runtime.gpu) {
        m.published.state = 0u;
        m.published.reason = "Soft bodies run only on PhysX's GPU solvers";
        m.frameOut.count = 0;
        return;
    }
    if (buildingsAct == UINT32_MAX || buildings.empty() || !haveAir) {
        if (!m.scene) { m.published.state = 3u; m.frameOut.count = 0; }
        if (m.scene && buildingsAct != m.act) { m.releaseScene(); m.frameOut.count = 0; }
        return;
    }
    if (m.scene && buildingsAct != m.act) m.releaseScene();
    if (!m.scene) {
        const auto began = std::chrono::steady_clock::now();
        if (!m.buildScene(buildings)) {
            m.releaseScene();
            m.published.state = 0u;
            m.published.reason = "PhysX soft-body scene creation failed";
            return;
        }
        m.act = buildingsAct;
        for (size_t i = 0; i < m.microbes.size(); ++i) m.place(m.microbes[i], air, uint32_t(i));
        m.published.backend = std::string("NVIDIA PhysX ") + runtime.sdkVersion + ", GPU deformable volumes on " + runtime.cudaDevice;
        BOO_LOG_INFO("PhysX", "Soft bodies: " + std::to_string(m.microbes.size()) + " microbes of " +
                              std::to_string(m.mesh->getCollisionMesh()->getNbTetrahedrons()) + " collision and " +
                              std::to_string(m.mesh->getSimulationMesh()->getNbTetrahedrons()) + " simulation tetrahedra each, over " +
                              std::to_string(m.statics.size() - 1) + " buildings, built in " +
                              std::to_string(static_cast<int>(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - began).count())) + " ms");
    }
    m.published.state = 2u;
    m.published.reason.clear();

    auto* cuda = static_cast<PxCudaContextManager*>(m.runtime.cudaContextManager());
    const auto t0 = std::chrono::steady_clock::now();
    if (m.simulating) {
        m.scene->fetchResults(true);
        m.simulating = false;
    }
    const auto tFetched = std::chrono::steady_clock::now();
    PxSimulationStatistics statistics;
    m.scene->getSimulationStatistics(statistics);

    // Read every microbe's collision mesh back and fit its ellipsoid.
    const uint32_t n = static_cast<uint32_t>(m.microbes.size());
    const uint32_t collVerts = m.mesh->getCollisionMesh()->getNbVertices();
    m.readback.resize(size_t(collVerts) * n);
    {
        // Every microbe's collision mesh in one go, under one lock.
        PxScopedCudaLock lock(*cuda);
        for (uint32_t i = 0; i < n; ++i)
            cuda->getCudaContext()->memcpyDtoHAsync(m.readback.data() + size_t(i) * collVerts,
                                                    reinterpret_cast<CUdeviceptr>(m.microbes[i].body->getPositionInvMassBufferD()), collVerts * sizeof(PxVec4), CUstream(0));
        cuda->getCudaContext()->streamSynchronize(CUstream(0));
    }
    m.poses.resize(size_t(n) * 8u);
    m.stretch.resize(size_t(n) * 3u);
    std::vector<float> lastPoses = m.poses, lastStretch = m.stretch;
    uint32_t resting = 0;
    float widestSquash = 0.0f;
    for (uint32_t i = 0; i < n; ++i) {
        auto& microbe = m.microbes[i];
        const PxVec4* verts = m.readback.data() + size_t(i) * collVerts;
        PxVec3 mean(0.0f);
        for (uint32_t v = 0; v < collVerts; ++v) mean += PxVec3(verts[v].x, verts[v].y, verts[v].z);
        mean *= 1.0f / float(collVerts);
        float cov[3][3]{};
        for (uint32_t v = 0; v < collVerts; ++v) {
            const PxVec4& p = verts[v];
            const float d[3] = {p.x - mean.x, p.y - mean.y, p.z - mean.z};
            for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) cov[r][c] += d[r] * d[c];
        }
        float axes[3][3];
        symmetricEigen(cov, axes);
        if (PxVec3(axes[0][0], axes[1][0], axes[2][0]).cross(PxVec3(axes[0][1], axes[1][1], axes[2][1])).dot(PxVec3(axes[0][2], axes[1][2], axes[2][2])) < 0.0f)
            for (int r = 0; r < 3; ++r) axes[r][2] = -axes[r][2]; // right-handed
        // Half-extents along each principal axis, and the centre of those extents.
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (uint32_t v = 0; v < collVerts; ++v) {
            const PxVec4& p = verts[v];
            const float d[3] = {p.x - mean.x, p.y - mean.y, p.z - mean.z};
            for (int a = 0; a < 3; ++a) {
                const float s = d[0] * axes[0][a] + d[1] * axes[1][a] + d[2] * axes[2][a];
                lo[a] = std::min(lo[a], s); hi[a] = std::max(hi[a], s);
            }
        }
        PxVec3 centre = mean;
        for (int a = 0; a < 3; ++a) centre += PxVec3(axes[0][a], axes[1][a], axes[2][a]) * (0.5f * (lo[a] + hi[a]));
        const PxQuat q = PxQuat(PxMat33(PxVec3(axes[0][0], axes[1][0], axes[2][0]), PxVec3(axes[0][1], axes[1][1], axes[2][1]),
                                        PxVec3(axes[0][2], axes[1][2], axes[2][2]))).getNormalized();
        float* pose = m.poses.data() + size_t(i) * 8u;
        const float now[8] = {centre.x, centre.y, centre.z, q.x, q.y, q.z, q.w, 1.0f};
        std::copy(now, now + 8, pose);
        float* s = m.stretch.data() + size_t(i) * 3u;
        float shortest = 1e30f, longest = 0.0f;
        for (int a = 0; a < 3; ++a) {
            s[a] = std::max(0.02f, 0.5f * (hi[a] - lo[a]));
            shortest = std::min(shortest, s[a]); longest = std::max(longest, s[a]);
        }
        widestSquash = std::max(widestSquash, 1.0f - shortest / std::max(longest, 1e-3f));
        // Resting: centre no longer moving.
        const float* before = lastPoses.size() == m.poses.size() ? lastPoses.data() + size_t(i) * 8u : pose;
        const float moved = std::sqrt((pose[0] - before[0]) * (pose[0] - before[0]) + (pose[1] - before[1]) * (pose[1] - before[1]) +
                                      (pose[2] - before[2]) * (pose[2] - before[2]));
        // Judged only across a step: at high frame rates many frames take none.
        if (m.stepsLast > 0) microbe.resting = m.havePrevious && moved < 0.002f * float(m.stepsLast);
        if (microbe.resting || centre.y < m.groundY - 5.0f) ++resting;
        // A body the solver has lost (a deep impact can do it) is dropped in afresh, not drawn.
        if (!std::isfinite(centre.x + centre.y + centre.z) || longest > 6.0f * microbe.radius) {
            m.place(microbe, air, uint32_t(m.microbes.size()) + i); // a ring above the others
            for (int a = 0; a < 3; ++a) s[a] = 1e-3f;
            ++m.lost;
        }
    }
    if (m.havePrevious && lastPoses.size() == m.poses.size()) { m.previous = lastPoses; m.previousStretch = lastStretch; }
    else { m.previous = m.poses; m.previousStretch = m.stretch; }
    m.havePrevious = true;
    m.frameOut.count = n;
    m.frameOut.poses = m.poses.data();
    m.frameOut.previousPoses = m.previous.data();
    m.frameOut.stretch = m.stretch.data();
    m.frameOut.previousStretch = m.previousStretch.data();
    const uint32_t start[7] = {0u, 0u, n, n, n, n, n}; // all celadon (look 1)
    std::copy(start, start + 7, m.frameOut.lookStart);

    // Half a minute after the shower settles, the microbes rise for the next one.
    const uint32_t steps = m.stepper.advance(frameSeconds, paused, false);
    if (!paused) {
        if (resting == n) m.sinceSettled += std::min(frameSeconds, 0.1);
        if (m.sinceSettled > 30.0) {
            for (size_t i = 0; i < m.microbes.size(); ++i) m.place(m.microbes[i], air, uint32_t(i));
            m.sinceSettled = 0.0;
        }
    }
    for (uint32_t step = 0; step + 1 < steps; ++step) {
        m.scene->simulate(static_cast<PxReal>(m.stepper.stepSeconds()));
        m.scene->fetchResults(true);
    }
    if (steps > 0) {
        m.scene->simulate(static_cast<PxReal>(m.stepper.stepSeconds()));
        m.simulating = true;
    }
    m.stepsLast = steps;
    const auto t1 = std::chrono::steady_clock::now();
    auto smooth = [](float& v, float sample) { v = v > 0.0f ? v * 0.9f + sample * 0.1f : sample; };
    smooth(m.published.stepWallMs, std::chrono::duration<float, std::milli>(tFetched - t0).count() +
                                       (steps > 1 ? std::chrono::duration<float, std::milli>(t1 - tFetched).count() : 0.0f));
    smooth(m.published.stepCpuMs, std::chrono::duration<float, std::milli>(t1 - tFetched).count());
    m.published.counters[0] = static_cast<float>(n);
    m.published.counters[1] = static_cast<float>(m.mesh->getSimulationMesh()->getNbTetrahedrons() * n);
    m.published.counters[2] = widestSquash * 100.0f;
    m.published.counters[3] = static_cast<float>(m.statics.size() - 1);
    m.published.counterNames[0] = "microbes";
    m.published.counterNames[1] = "tetrahedra";
    m.published.counterNames[2] = "widest_squash_percent";
    m.published.counterNames[3] = "buildings";
    m.published.gpuMemoryMegabytes = static_cast<float>(double(statistics.gpuMemHeap) / (1024.0 * 1024.0));
    m.published.steps = m.stepper.totalSteps();
#endif
}

} // namespace boo
