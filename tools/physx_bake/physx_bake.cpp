// boo_physx_bake: offline PhysX simulation for the film (docs/physx_suite_plan.md, step 4).
//
// Each bake runs the real PhysX SDK on the CPU with enhanced determinism, at a fixed 1/120 s step,
// and writes a track of poses against film time into assets/physics. The film plays the track back
// with no physics library (include/boo/baked_physics.hpp). Re-running a bake is a content change:
// commit the new file, refresh the asset hashes, and regenerate the film's reference captures.
//
//   boo_physx_bake winch [output.bphys]   the parcel case swinging under the courier drone (2:43–2:53.5)
//   boo_physx_bake iris [output.bphys]    Madame N's iris as a geared mechanism, over its opening range
//   boo_physx_bake damper [output.bphys]  Taipei 101's tuned mass damper swinging in the tower, the whole film
//   boo_physx_bake cloth [output.bphys]   the canopy ribbons as GPU cloth, where they are on screen (needs CUDA)

#include "boo/baked_physics.hpp"
#include "boo/canopy_cloth.hpp"
#include "boo/city_modules.hpp"
#include "boo/wind_field.hpp"

#pragma warning(push, 0)
#include "PxPhysicsAPI.h"
#include "extensions/PxDeformableSurfaceExt.h"
#include "extensions/PxCudaHelpersExt.h"
#include "cudamanager/PxCudaContext.h"
#pragma warning(pop)

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace physx;

namespace {

PxDefaultAllocator gAllocator;
PxDefaultErrorCallback gErrors;

constexpr double kStep = 1.0 / 120.0;
using PhysxPhysicsRef = PxPhysics&;

boo::BakedPose poseOf(const PxTransform& t) {
    return {{t.p.x, t.p.y, t.p.z}, {t.q.x, t.q.y, t.q.z, t.q.w}};
}

// The parcel case, hung by its line from the courier drone as the drone flies the authored route and
// pays the line out. The authored film has the case hang rigidly below the drone; here it swings,
// pushed by the drone's own accelerations and by the shared terrace wind. In the last second before
// it lands the swing is blended out onto the authored path, so it settles exactly on the dock.
boo::BakedTrack bakeWinch(PxPhysics& physics, PxCpuDispatcher& dispatcher) {
    constexpr float kStart = 163.0f, kLand = 173.5f, kBlend = 1.0f;
    constexpr float kAttach = 0.26f; // the line meets the case latch this far above its centre
    PxSceneDesc desc(physics.getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = &dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    desc.flags |= PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
    PxScene* scene = physics.createScene(desc);
    PxMaterial* material = physics.createMaterial(0.6f, 0.5f, 0.1f);

    auto authored = [](float t) { return boo::sanctuaryParcelPose(t, 1.0f); };
    auto yawQuat = [](float yaw) { return PxQuat(yaw, PxVec3(0.0f, 1.0f, 0.0f)); };

    const auto first = authored(kStart);
    PxRigidDynamic* drone = physics.createRigidDynamic(PxTransform(PxVec3(first.lineOrigin[0], first.lineOrigin[1], first.lineOrigin[2])));
    drone->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
    scene->addActor(*drone);
    PxRigidDynamic* box = PxCreateDynamic(physics, PxTransform(PxVec3(first.center[0], first.center[1], first.center[2]), yawQuat(first.yaw)),
                                          PxBoxGeometry(0.36f, 0.22f, 0.27f), *material, 60.0f); // about 5 kg
    box->setLinearDamping(0.05f);
    box->setAngularDamping(0.6f);
    box->setSolverIterationCounts(16, 4);
    scene->addActor(*box);
    // Start moving with the drone, so the swing comes from changes in its motion, not a jolt.
    {
        const auto a = authored(kStart), b = authored(kStart + 0.01f);
        box->setLinearVelocity(PxVec3(b.center[0] - a.center[0], b.center[1] - a.center[1], b.center[2] - a.center[2]) * 100.0f);
    }
    PxDistanceJoint* line = PxDistanceJointCreate(physics, drone, PxTransform(PxIdentity), box, PxTransform(PxVec3(0.0f, kAttach, 0.0f)));
    line->setDistanceJointFlag(PxDistanceJointFlag::eMAX_DISTANCE_ENABLED, true);
    line->setDistanceJointFlag(PxDistanceJointFlag::eSPRING_ENABLED, false);
    line->setTolerance(0.002f);

    boo::BakedTrack track;
    track.name = "parcel_case";
    track.firstTime = kStart;
    track.keyRate = static_cast<float>(1.0 / kStep);
    float maxSwing = 0.0f;
    for (int i = 0;; ++i) {
        const float t = kStart + float(i * kStep);
        if (t > kLand + 1e-4f) break;
        const auto now = authored(t);
        // Record this step's pose, blended onto the authored path over the last second.
        PxTransform pose = box->getGlobalPose();
        const float swing = std::hypot(pose.p.x - now.center[0], pose.p.z - now.center[2]);
        maxSwing = std::max(maxSwing, swing);
        const float u = std::clamp((t - (kLand - kBlend)) / kBlend, 0.0f, 1.0f);
        const float w = u * u * (3.0f - 2.0f * u);
        const PxVec3 rest(now.center[0], now.center[1], now.center[2]);
        PxQuat restQ = yawQuat(now.yaw);
        PxQuat q = pose.q;
        if (q.dot(restQ) < 0.0f) restQ = -restQ;
        boo::BakedPose key = poseOf(PxTransform(pose.p * (1.0f - w) + rest * w, (q * (1.0f - w) + restQ * w).getNormalized()));
        track.keys.push_back(key);

        // Advance: the drone follows the authored route, the line pays out, the wind pushes the case.
        const auto next = authored(t + float(kStep));
        drone->setKinematicTarget(PxTransform(PxVec3(next.lineOrigin[0], next.lineOrigin[1], next.lineOrigin[2])));
        line->setMaxDistance(std::max(0.02f, next.lineLength));
        const float at[3] = {pose.p.x, pose.p.y, pose.p.z};
        const auto wind = boo::windAt(at, t);
        const PxVec3 relative = PxVec3(wind[0], wind[1], wind[2]) - box->getLinearVelocity();
        box->addForce(relative * (0.5f * 1.2f * 1.05f * 0.25f * relative.magnitude())); // drag on a quarter square metre
        scene->simulate(static_cast<PxReal>(kStep));
        scene->fetchResults(true);
    }
    std::printf("winch: %zu keys from %.2f s to %.2f s, widest swing %.2f m\n", track.keys.size(), track.firstTime, track.lastTime(), maxSwing);
    line->release();
    scene->removeActor(*box);
    scene->removeActor(*drone);
    box->release();
    drone->release();
    scene->release();
    material->release();
    return track;
}

// Madame N's iris (parts 220..223 of microbial_sanctuary in tools/gen_trilogy_meshes.py) as a
// mechanism: an actuator ring turns on a revolute joint about the iris axis, and each of the four vanes
// turns on its own revolute joint at the bronze knuckle where its rail begins, coupled to the ring by a
// gear joint. The iris is driven live by the stage director's mood, so the "time" axis of these tracks is
// the iris opening, 0 to 1, in 100 steps; the film looks the vanes up from the live value.
// Iris model space: the iris axis is +Y and the vanes lie in the XZ plane.
std::vector<boo::BakedTrack> bakeIris(PhysxPhysicsRef physics, PxCpuDispatcher& dispatcher);
} // namespace

namespace {
std::vector<boo::BakedTrack> bakeIris(PhysxPhysicsRef physics, PxCpuDispatcher& dispatcher) {
    constexpr int kSamples = 100;
    constexpr float kRingTravel = 0.42f;   // radians of actuator ring turn from closed to open
    constexpr float kGearRatio = 0.85f;    // vane turn per ring turn, alternating in sense
    PxSceneDesc desc(physics.getTolerancesScale());
    desc.gravity = PxVec3(0.0f);
    desc.cpuDispatcher = &dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    desc.flags |= PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
    PxScene* scene = physics.createScene(desc);
    PxMaterial* material = physics.createMaterial(0.5f, 0.5f, 0.0f);
    // Revolute joints turn about their local X axis: turn X onto the iris axis, +Y.
    const PxQuat xToY(PxHalfPi, PxVec3(0.0f, 0.0f, 1.0f));

    PxRigidDynamic* ring = PxCreateDynamic(physics, PxTransform(PxIdentity), PxBoxGeometry(2.45f, 0.02f, 2.45f), *material, 20.0f);
    scene->addActor(*ring);
    PxRevoluteJoint* ringHinge = PxRevoluteJointCreate(physics, nullptr, PxTransform(PxVec3(0.0f), xToY), ring, PxTransform(PxVec3(0.0f), xToY));
    ringHinge->setRevoluteJointFlag(PxRevoluteJointFlag::eDRIVE_ENABLED, true);

    struct Vane { PxRigidDynamic* body; PxRevoluteJoint* hinge; PxGearJoint* gear; PxVec3 pivot; PxQuat rest; };
    std::vector<Vane> vanes;
    for (int k = 0; k < 4; ++k) {
        const float a0 = 0.16f + float(k) * 0.72f;
        const PxVec3 pivot(2.45f * std::cos(a0), -0.11f, 2.45f * std::sin(a0));  // the knuckle at rail point 0
        const float mid = a0 + 0.33f;
        const PxVec3 centre(2.40f * std::cos(mid), -0.10f, 2.40f * std::sin(mid));
        PxRigidDynamic* body = PxCreateDynamic(physics, PxTransform(centre, PxQuat(-mid, PxVec3(0.0f, 1.0f, 0.0f))),
                                               PxBoxGeometry(0.80f, 0.02f, 0.09f), *material, 7800.0f * 0.02f);
        scene->addActor(*body);
        const PxTransform worldPivot(pivot, xToY);
        PxRevoluteJoint* hinge = PxRevoluteJointCreate(physics, nullptr, worldPivot, body, body->getGlobalPose().getInverse() * worldPivot);
        PxGearJoint* gear = PxGearJointCreate(physics, ring, PxTransform(PxVec3(0.0f), xToY), body, body->getGlobalPose().getInverse() * worldPivot);
        gear->setHinges(ringHinge, hinge);
        gear->setGearRatio(k & 1 ? -kGearRatio : kGearRatio);
        vanes.push_back({body, hinge, gear, pivot, body->getGlobalPose().q});
    }

    std::vector<boo::BakedTrack> tracks(4);
    for (int k = 0; k < 4; ++k) {
        tracks[size_t(k)].name = "iris_vane_" + std::to_string(k);
        tracks[size_t(k)].firstTime = 0.0f;
        tracks[size_t(k)].keyRate = float(kSamples);
    }
    float widest = 0.0f;
    for (int i = 0; i <= kSamples; ++i) {
        // Drive the ring to this opening and let the mechanism settle before recording.
        const float target = kRingTravel * float(i) / float(kSamples);
        for (int s = 0; s < 120; ++s) {
            const float error = target - ringHinge->getAngle();
            ringHinge->setDriveVelocity(error * 30.0f);
            scene->simulate(static_cast<PxReal>(kStep));
            scene->fetchResults(true);
        }
        for (int k = 0; k < 4; ++k) {
            const Vane& v = vanes[size_t(k)];
            const PxQuat turn = v.body->getGlobalPose().q * v.rest.getConjugate(); // turn about the pivot since rest
            tracks[size_t(k)].keys.push_back({{v.pivot.x, v.pivot.y, v.pivot.z}, {turn.x, turn.y, turn.z, turn.w}});
            float angle = 0.0f; PxVec3 axis;
            turn.toRadiansAndUnitAxis(angle, axis);
            widest = std::max(widest, angle);
        }
    }
    std::printf("iris: 4 vanes x %d openings, ring %.2f rad, widest vane turn %.3f rad\n", kSamples + 1, kRingTravel, widest);
    for (auto& v : vanes) { v.gear->release(); v.hinge->release(); scene->removeActor(*v.body); v.body->release(); }
    ringHinge->release();
    scene->removeActor(*ring);
    ring->release();
    scene->release();
    material->release();
    return tracks;
}
} // namespace

namespace {
// Taipei 101's 660-tonne tuned mass damper: a steel ball 5.5 m across hung on cables 17.3 m long from
// the 92nd floor, with hydraulic dashpots beneath. The breeze sways the tower a little; the ball, tuned
// to the tower's own period, swings against it and the dashpots turn that motion into heat. Here: the
// tower top as a kinematic body swaying with the shared terrace wind, the ball on a spherical joint
// (the cable bundle) from it, and the dashpots as damping drives on that joint's swing. Kit space, Y up.
boo::BakedTrack bakeDamper(PxPhysics& physics, PxCpuDispatcher& dispatcher) {
    constexpr float kLength = 426.8f - 409.5f, kEnd = 360.0f;
    constexpr float kMass = 660000.0f;
    PxSceneDesc desc(physics.getTolerancesScale());
    desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
    desc.cpuDispatcher = &dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    desc.flags |= PxSceneFlag::eENABLE_ENHANCED_DETERMINISM;
    PxScene* scene = physics.createScene(desc);
    PxMaterial* material = physics.createMaterial(0.5f, 0.5f, 0.0f);
    const PxVec3 pivot(0.0f, 426.8f, 0.0f);
    PxRigidDynamic* tower = physics.createRigidDynamic(PxTransform(pivot));
    tower->setRigidBodyFlag(PxRigidBodyFlag::eKINEMATIC, true);
    scene->addActor(*tower);
    PxRigidDynamic* ball = PxCreateDynamic(physics, PxTransform(PxVec3(0.0f, 409.5f, 0.0f)), PxSphereGeometry(2.75f), *material, 1.0f);
    PxRigidBodyExt::setMassAndUpdateInertia(*ball, kMass);
    scene->addActor(*ball);
    PxD6Joint* cables = PxD6JointCreate(physics, tower, PxTransform(PxIdentity), ball, PxTransform(PxVec3(0.0f, kLength, 0.0f)));
    cables->setMotion(PxD6Axis::eSWING1, PxD6Motion::eFREE);
    cables->setMotion(PxD6Axis::eSWING2, PxD6Motion::eFREE);
    cables->setMotion(PxD6Axis::eTWIST, PxD6Motion::eLOCKED);
    // Dashpots: pure damping on the swing, about a tenth of critical for this pendulum.
    const float omega = std::sqrt(9.81f / kLength);
    const float damping = 0.2f * 0.10f * kMass * kLength * kLength * omega;
    cables->setAngularDriveConfig(PxD6AngularDriveConfig::eSWING_TWIST);
    cables->setDrive(PxD6Drive::eSWING1, PxD6JointDrive(0.0f, damping, PX_MAX_F32, false));
    cables->setDrive(PxD6Drive::eSWING2, PxD6JointDrive(0.0f, damping, PX_MAX_F32, false));

    boo::BakedTrack track;
    track.name = "taipei_damper";
    track.firstTime = 0.0f;
    track.keyRate = 60.0f; // a slow swing: sixty keys a second is plenty
    const int stepsPerKey = int(std::lround(1.0 / kStep / track.keyRate));
    float widest = 0.0f;
    for (int i = 0;; ++i) {
        const float t = float(i * kStep);
        if (t > kEnd + 1e-4f) break;
        if (i % stepsPerKey == 0) {
            // The ball's turn about the pivot, relative to the tower: what the film applies to the ball.
            const PxTransform towerPose = tower->getGlobalPose();
            const PxVec3 hang = (ball->getGlobalPose().p - towerPose.p).getNormalized();
            const PxQuat turn = PxShortestRotation(PxVec3(0.0f, -1.0f, 0.0f), hang);
            track.keys.push_back({{towerPose.p.x - pivot.x, towerPose.p.y - pivot.y, towerPose.p.z - pivot.z}, {turn.x, turn.y, turn.z, turn.w}});
            widest = std::max(widest, kLength * std::sqrt(hang.x * hang.x + hang.z * hang.z));
        }
        // The tower top sways downwind with the gusts: a few centimetres, as in a breeze.
        const float at[3] = {0.0f, 409.5f, 0.0f};
        const auto wind = boo::windAt(at, t + float(kStep));
        const float sway = 0.03f * std::sqrt(wind[0] * wind[0] + wind[2] * wind[2]) / boo::kWindMetresPerSecond;
        tower->setKinematicTarget(PxTransform(pivot + PxVec3(boo::kWindHeading[0], 0.0f, boo::kWindHeading[2]) * sway));
        scene->simulate(static_cast<PxReal>(kStep));
        scene->fetchResults(true);
    }
    std::printf("damper: %zu keys over %.0f s, widest swing of the ball %.3f m\n", track.keys.size(), kEnd, widest);
    cables->release();
    scene->removeActor(*ball);
    scene->removeActor(*tower);
    ball->release();
    tower->release();
    scene->release();
    material->release();
    return track;
}
} // namespace

namespace {
// The canopy ribbons as cloth (include/boo/canopy_cloth.hpp): for each lamella, a strip of woven
// fabric 12% wider than the gap between its two bronze tendons, pinned along both tendons and at both
// feet. Gravity and the terrace wind settle and billow the slack. PhysX's cloth solver (a deformable
// surface) runs only on the GPU, so this bake needs CUDA; GPU results can differ between GPUs and
// drivers, so the committed file is the truth and a re-bake is a content change.
std::vector<boo::BakedGridTrack> bakeCloth(PxFoundation& foundation, PxPhysics& physics, PxCpuDispatcher& dispatcher) {
    using namespace boo;
    constexpr float kSlack = 1.12f;        // fabric width over the gap between the tendons
    constexpr float kThickness = 0.004f;   // woven silk-glass
    constexpr float kArealDensity = 0.30f; // kilograms a square metre
    constexpr float kWarmup = 12.0f;       // seconds to settle before each span starts
    PxCudaContextManagerDesc cudaDesc;
    PxCudaContextManager* cuda = PxCreateCudaContextManager(foundation, cudaDesc, PxGetProfilerCallback());
    if (!cuda || !cuda->contextIsValid()) {
        std::printf("cloth: PhysX could not start CUDA; the cloth solver runs only on the GPU\n");
        if (cuda) cuda->release();
        return {};
    }
    PxCookingParams cooking(physics.getTolerancesScale());
    cooking.meshWeldTolerance = 0.0f;
    cooking.buildTriangleAdjacencies = false;
    cooking.buildGPUData = true;
    cooking.midphaseDesc = PxMeshMidPhase::eBVH34;
    cooking.meshPreprocessParams = PxMeshPreprocessingFlag::eENABLE_VERT_MAPPING;

    const int nodes = kCanopySimU * kCanopySimV;
    auto at = [](int i, int j) { return i * kCanopySimV + j; };
    std::vector<PxU32> triangles;
    for (int i = 1; i < kCanopySimU; ++i)
        for (int j = 1; j < kCanopySimV; ++j) {
            triangles.insert(triangles.end(), {PxU32(at(i - 1, j - 1)), PxU32(at(i, j - 1)), PxU32(at(i - 1, j))});
            triangles.insert(triangles.end(), {PxU32(at(i - 1, j)), PxU32(at(i, j - 1)), PxU32(at(i, j))});
        }
    auto layout = [&](int ribbon, float widen, std::vector<PxVec3>& out) {
        out.resize(size_t(nodes));
        for (int i = 0; i < kCanopySimU; ++i)
            for (int j = 0; j < kCanopySimV; ++j) {
                const float u = float(i) / float(kCanopySimU - 1), v = (float(j) / float(kCanopySimV - 1)) * 2.0f - 1.0f;
                const auto p = canopyRibbonPoint(ribbon, u, v * widen);
                out[size_t(at(i, j))] = PxVec3(p[0], p[1], p[2]);
            }
    };
    auto pinned = [&](int i, int j) { return i == 0 || i == kCanopySimU - 1 || j == 0 || j == kCanopySimV - 1; };

    std::vector<BakedGridTrack> tracks;
    float widestBillow = 0.0f;
    for (const CanopySpan& span : kCanopySpans) {
        PxSceneDesc desc(physics.getTolerancesScale());
        desc.gravity = PxVec3(0.0f, -9.81f, 0.0f);
        desc.cpuDispatcher = &dispatcher;
        desc.filterShader = PxDefaultSimulationFilterShader;
        desc.cudaContextManager = cuda;
        desc.flags |= PxSceneFlag::eENABLE_GPU_DYNAMICS; // enhanced determinism was tried: GPU cloth still differs run to run
        desc.broadPhaseType = PxBroadPhaseType::eGPU;
        desc.solverType = PxSolverType::eTGS;
        PxScene* scene = physics.createScene(desc);
        PxDeformableSurfaceMaterial* fabric = physics.createDeformableSurfaceMaterial(2.0e8f, 0.3f, 0.4f, kThickness, 2.0e-4f);
        fabric->setBendingDamping(0.02f);

        struct Strip { PxDeformableSurface* body; PxVec4* pos; PxVec4* vel; std::vector<PxVec3> layout; std::vector<float> invMass; };
        std::vector<Strip> strips(kCanopyRibbons);
        for (int r = 0; r < kCanopyRibbons; ++r) {
            Strip& s = strips[size_t(r)];
            std::vector<PxVec3> rest, start, velocity(size_t(nodes), PxVec3(0.0f));
            layout(r, 1.0f, s.layout);
            layout(r, kSlack, rest);
            start = s.layout;
            // Start the free fabric a few millimetres below the tendons, so it buckles downwards.
            for (int i = 0; i < kCanopySimU; ++i)
                for (int j = 0; j < kCanopySimV; ++j)
                    if (!pinned(i, j)) start[size_t(at(i, j))].y -= 0.005f;
            PxTriangleMeshDesc mesh;
            mesh.points.count = PxU32(nodes);
            mesh.points.stride = sizeof(PxVec3);
            mesh.points.data = rest.data();
            mesh.triangles.count = PxU32(triangles.size() / 3);
            mesh.triangles.stride = 3 * sizeof(PxU32);
            mesh.triangles.data = triangles.data();
            PxTriangleMesh* triangleMesh = PxCreateTriangleMesh(cooking, mesh, physics.getPhysicsInsertionCallback());
            if (!triangleMesh) { std::printf("cloth: could not cook ribbon %d\n", r); return {}; }
            s.body = physics.createDeformableSurface(*cuda);
            PxDeformableSurfaceMaterial* materials[] = {fabric};
            PxShape* shape = physics.createShape(PxTriangleMeshGeometry(triangleMesh), reinterpret_cast<PxDeformableSurfaceMaterial**>(materials), 1, true,
                                                 PxShapeFlag::eSIMULATION_SHAPE);
            s.body->attachShape(*shape);
            shape->setContactOffset(2.0f * kThickness);
            shape->setRestOffset(kThickness);
            shape->release();
            triangleMesh->release();
            scene->addActor(*s.body);
            PxVec4* restPinned = nullptr;
            PxDeformableSurfaceExt::allocateAndInitializeHostMirror(*s.body, start.data(), velocity.data(), rest.data(), 1.0f, PxTransform(PxIdentity), cuda,
                                                                    s.pos, s.vel, restPinned);
            PxDeformableSurfaceExt::distributeDensityToVertices(*s.body, kArealDensity / kThickness, kThickness, s.pos);
            for (int i = 0; i < kCanopySimU; ++i)
                for (int j = 0; j < kCanopySimV; ++j)
                    if (pinned(i, j)) s.pos[at(i, j)].w = 0.0f; // held by the tendons and the feet
            s.invMass.resize(size_t(nodes));
            for (int n = 0; n < nodes; ++n) s.invMass[size_t(n)] = s.pos[n].w;
            PxDeformableSurfaceExt::copyToDevice(*s.body, PxDeformableSurfaceDataFlag::eALL, PxU32(nodes), s.pos, s.vel, restPinned);
            PX_EXT_PINNED_MEMORY_FREE(*cuda, restPinned);
            s.body->setLinearDamping(0.02f);
            s.body->setSolverIterationCounts(16, 1);
            s.body->setDeformableBodyFlag(PxDeformableBodyFlag::eDISABLE_SELF_COLLISION, true);
            s.body->setSettlingThreshold(0.0f);
            s.body->setSleepThreshold(0.0f);
        }

        BakedGridTrack track;
        track.name = span.track;
        track.firstTime = span.first;
        track.keyRate = kCanopyKeyRate;
        track.offsetScale = kCanopyOffsetScale;
        track.nodes = kCanopyTrackNodes;
        const int stepsPerKey = int(std::lround(1.0 / kStep / kCanopyKeyRate));
        const float start = span.first - kWarmup;
        std::vector<PxVec3> normals(static_cast<size_t>(nodes));
        for (int step = 0;; ++step) {
            const float t = start + float(step * kStep);
            if (t > span.last + 1e-4f) break;
            {
                PxScopedCudaLock lock(*cuda);
                for (Strip& s : strips) {
                    cuda->getCudaContext()->memcpyDtoH(s.pos, reinterpret_cast<CUdeviceptr>(s.body->getPositionInvMassBufferD()), nodes * sizeof(PxVec4));
                    cuda->getCudaContext()->memcpyDtoH(s.vel, reinterpret_cast<CUdeviceptr>(s.body->getVelocityBufferD()), nodes * sizeof(PxVec4));
                }
            }
            const bool record = t >= span.first - 1e-4f && (step - int(std::lround(kWarmup / kStep))) % stepsPerKey == 0;
            if (record) {
                for (int r = 0; r < kCanopyRibbons; ++r) {
                    const Strip& s = strips[size_t(r)];
                    for (int gi = 0; gi < kCanopyGridU; ++gi)
                        for (int gj = 0; gj < kCanopyGridV; ++gj) {
                            const int n = at(gi * 2, gj * 2);
                            const PxVec3 d = PxVec3(s.pos[n].x, s.pos[n].y, s.pos[n].z) - s.layout[size_t(n)];
                            widestBillow = std::max(widestBillow, d.magnitude());
                            for (float c : {d.x, d.y, d.z})
                                track.offsets.push_back(int16_t(std::clamp(std::lround(c / kCanopyOffsetScale), -32767l, 32767l)));
                        }
                }
            }
            // The wind on the fabric: the pressure of the air moving through each node's patch of cloth,
            // along the cloth's normal, from the shared terrace breeze.
            for (Strip& s : strips) {
                for (int i = 0; i < kCanopySimU; ++i)
                    for (int j = 0; j < kCanopySimV; ++j) {
                        const int n = at(i, j);
                        auto p = [&](int a, int b) { const PxVec4& q = s.pos[at(std::clamp(a, 0, kCanopySimU - 1), std::clamp(b, 0, kCanopySimV - 1))]; return PxVec3(q.x, q.y, q.z); };
                        normals[size_t(n)] = (p(i + 1, j) - p(i - 1, j)).cross(p(i, j + 1) - p(i, j - 1)).getNormalized();
                    }
                const float patch = (9.2f * 0.66f * kSlack) / float(nodes);
                for (int n = 0; n < nodes; ++n) {
                    if (s.invMass[size_t(n)] == 0.0f) continue;
                    const float here[3] = {s.pos[n].x, s.pos[n].y, s.pos[n].z};
                    const auto w = windAt(here, t);
                    const PxVec3 relative = PxVec3(w[0], w[1], w[2]) - PxVec3(s.vel[n].x, s.vel[n].y, s.vel[n].z);
                    const PxVec3 nrm = normals[size_t(n)];
                    const float across = relative.dot(nrm);
                    const PxVec3 force = nrm * (0.5f * 1.2f * 1.2f * patch * across * std::fabs(across));
                    const PxVec3 dv = force * (s.invMass[size_t(n)] * float(kStep));
                    s.vel[n].x += dv.x; s.vel[n].y += dv.y; s.vel[n].z += dv.z;
                }
            }
            {
                PxScopedCudaLock lock(*cuda);
                for (Strip& s : strips) {
                    cuda->getCudaContext()->memcpyHtoD(reinterpret_cast<CUdeviceptr>(s.body->getVelocityBufferD()), s.vel, nodes * sizeof(PxVec4));
                    s.body->markDirty(PxDeformableSurfaceDataFlag::eVELOCITY);
                }
            }
            scene->simulate(static_cast<PxReal>(kStep));
            scene->fetchResults(true);
        }
        std::printf("cloth: %s, %u keys from %.1f s to %.1f s\n", track.name.c_str(), track.keyCount(), track.firstTime, track.lastTime());
        tracks.push_back(std::move(track));
        for (Strip& s : strips) {
            scene->removeActor(*s.body);
            s.body->release();
            PX_EXT_PINNED_MEMORY_FREE(*cuda, s.pos);
            PX_EXT_PINNED_MEMORY_FREE(*cuda, s.vel);
        }
        fabric->release();
        scene->release();
    }
    std::printf("cloth: widest billow from the woven shape %.3f m\n", widestBillow);
    cuda->release();
    return tracks;
}
} // namespace

int main(int argc, char** argv) {
    const std::string what = argc > 1 ? argv[1] : "";
    if (what != "winch" && what != "iris" && what != "damper" && what != "cloth") {
        std::printf("usage: boo_physx_bake winch|iris|damper|cloth [output.bphys]\n");
        return 2;
    }
    PxFoundation* foundation = PxCreateFoundation(PX_PHYSICS_VERSION, gAllocator, gErrors);
    PxPhysics* physics = PxCreatePhysics(PX_PHYSICS_VERSION, *foundation, PxTolerancesScale(), false, nullptr);
    PxInitExtensions(*physics, nullptr);
    PxDefaultCpuDispatcher* dispatcher = PxDefaultCpuDispatcherCreate(0); // one thread: same result every run
    boo::BakedPhysics out;
    if (what == "winch") out.tracks.push_back(bakeWinch(*physics, *dispatcher));
    else if (what == "damper") out.tracks.push_back(bakeDamper(*physics, *dispatcher));
    else if (what == "cloth") {
        out.grids = bakeCloth(*foundation, *physics, *dispatcher);
        if (out.grids.empty()) return 1;
    }
    else out.tracks = bakeIris(*physics, *dispatcher);
    const std::string path = argc > 2 ? argv[2]
        : (what == "winch" ? "assets/physics/parcel_winch.bphys" : what == "damper" ? "assets/physics/taipei_damper.bphys"
           : what == "cloth" ? "assets/physics/canopy_cloth.bphys" : "assets/physics/iris_mechanism.bphys");
    std::string error;
    const bool ok = boo::saveBakedPhysics(path, out, &error);
    std::printf("%s %s\n", ok ? "wrote" : "could not write", path.c_str());
    PxCloseExtensions();
    dispatcher->release();
    physics->release();
    foundation->release();
    return ok ? 0 : 1;
}
