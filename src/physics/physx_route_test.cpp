#include "boo/physx_route_test.hpp"

#include "../render/bird_flight.hpp"
#include "boo/last_light.hpp"

#include <cmath>

#if defined(BOO_HAS_PHYSX)
#pragma warning(push, 0)
#include "PxPhysicsAPI.h"
#pragma warning(pop)
#endif

namespace boo {

BirdRouteReport sweepBirdRoutes(PhysxSuite& runtime, const std::vector<CityCollisionBox>& buildings, uint32_t act) {
    BirdRouteReport report;
#if !defined(BOO_HAS_PHYSX)
    (void)runtime; (void)buildings; (void)act;
    report.reason = "This build does not include the NVIDIA PhysX SDK";
    return report;
#else
    using namespace physx;
    auto* physics = static_cast<PxPhysics*>(runtime.physics());
    auto* dispatcher = static_cast<PxCpuDispatcher*>(runtime.dispatcher());
    if (!physics || !dispatcher) { report.reason = "PhysX runtime not ready"; return report; }
    if (buildings.empty()) { report.reason = "no buildings for this city"; return report; }

    PxSceneDesc desc(physics->getTolerancesScale());
    desc.cpuDispatcher = dispatcher;
    desc.filterShader = PxDefaultSimulationFilterShader;
    PxScene* scene = physics->createScene(desc);
    PxMaterial* material = physics->createMaterial(0.5f, 0.5f, 0.0f);
    std::vector<PxRigidStatic*> statics;
    for (const auto& b : buildings) {
        const PxTransform pose(PxVec3(b.center[0], b.center[1], b.center[2]),
                               PxQuat(b.rotation[0], b.rotation[1], b.rotation[2], b.rotation[3]).getNormalized());
        if (PxRigidStatic* actor = PxCreateStatic(*physics, pose, PxBoxGeometry(b.half[0], b.half[1], b.half[2]), *material)) {
            scene->addActor(*actor);
            statics.push_back(actor);
            const float top = b.center[1] + std::abs(b.half[1]);
            if (top > report.tallest) { report.tallest = top; report.tallestAt[0] = b.center[0]; report.tallestAt[1] = b.center[2]; }
        }
    }
    report.buildings = static_cast<uint32_t>(statics.size());

    // A bird as a sphere half its wingspan across (the 1.66 m model, at its scale), grown by the
    // required clearance: closer than that to a building counts as a hit.
    auto test = [&](const BirdFlightPose& pose, const std::string& who, float filmTime, bool authored) {
        ++report.samples;
        const float r = 0.5f * 1.66f * pose.scale + kBirdBuildingClearance;
        PxOverlapBuffer hit;
        PxQueryFilterData filter(PxQueryFlag::eSTATIC | PxQueryFlag::eANY_HIT);
        const PxVec3 at(pose.center[0], pose.center[1], pose.center[2]);
        if (authored) {
            // The film's own birds are never moved: measure how close they come instead.
            for (float probe = 0.25f; probe <= kBirdBuildingClearance + 0.5f * 1.66f; probe += 0.25f) {
                if (scene->overlap(PxSphereGeometry(probe), PxTransform(at), hit, filter)) {
                    report.authoredClosest = std::min(report.authoredClosest, probe - 0.5f * 1.66f * pose.scale);
                    break;
                }
            }
            hit = PxOverlapBuffer();
        }
        if (!scene->overlap(PxSphereGeometry(r), PxTransform(at), hit, filter)) return;
        if (hit.hasBlock && hit.block.actor) {
            // How far up this bird must go for the building below it to be its clearance away.
            const float top = hit.block.actor->getWorldBounds().maximum.y;
            const float lift = top + r - pose.center[1];
            float& need = report.liftByFlock[authored ? who : who.substr(0, who.find(" bird"))];
            need = std::max(need, lift);
        }
        if (authored) ++report.authoredHits; else ++report.workoutHits;
        // The film's birds are counted bird by bird, so a fix can be aimed at the one that strays.
        const std::string flock = authored ? who : who.substr(0, who.find(" bird"));
        ++report.hitsByFlock[flock];
        if (report.hitsByFlock[flock] <= 3u) {
            BirdRouteHit h;
            h.who = who;
            h.act = act;
            h.filmTime = filmTime;
            for (int i = 0; i < 3; ++i) h.position[i] = pose.center[i];
            report.examples.push_back(h);
        }
    };

    // The film's own birds: two full circuits, a sample every tenth of a second, both route sets.
    for (bool trilogy : {false, true}) {
        const BirdRoute route = birdRoute(act, trilogy);
        const float lap = 6.2831853f / route.speed;
        for (float t = route.anchor; t < route.anchor + 2.f * lap; t += 0.1f)
            for (uint32_t bird = 0; bird < kCityBirdCount; ++bird)
                test(birdFlightPose(act, bird, t, 1.f, trilogy), std::string("authored bird ") + std::to_string(bird) +
                                                                     (trilogy ? " (trilogy route)" : ""), t, true);
    }
    // For each of the film's birds that comes too close, the smallest move across and up (metres, from
    // where it flies now) that keeps it clear on both route sets: what kTaipeiFlockNudge records.
    for (uint32_t bird = 0; bird < kCityBirdCount; ++bird) {
        const bool strays = report.hitsByFlock.count("authored bird " + std::to_string(bird)) ||
                            report.hitsByFlock.count("authored bird " + std::to_string(bird) + " (trilogy route)");
        if (!strays) continue;
        const FlockMember base = cityFlockMember(act, bird);
        auto clear = [&](float across, float above) {
            FlockMember m = base;
            m.lateral += across;
            m.height += above;
            for (bool trilogy : {false, true}) {
                const BirdRoute route = birdRoute(act, trilogy);
                const float lap = 6.2831853f / route.speed;
                for (float t = route.anchor; t < route.anchor + 2.f * lap; t += 0.1f) {
                    const BirdFlightPose pose = birdFlightPoseOf(act, m, float(bird), t, 1.f, trilogy, 0.f);
                    PxOverlapBuffer hit;
                    if (scene->overlap(PxSphereGeometry(0.5f * 1.66f * pose.scale + kBirdBuildingClearance), PxTransform(PxVec3(pose.center[0], pose.center[1], pose.center[2])),
                                       hit, PxQueryFilterData(PxQueryFlag::eSTATIC | PxQueryFlag::eANY_HIT)))
                        return false;
                }
            }
            return true;
        };
        float bestCost = 1e9f, bestAcross = 0.f, bestAbove = 0.f;
        for (float above = 0.f; above <= 12.f; above += 0.25f)
            for (float across = -6.f; across <= 6.f; across += 0.25f) {
                const float cost = std::hypot(across, above);
                if (cost < bestCost && clear(across, above)) { bestCost = cost; bestAcross = across; bestAbove = above; }
            }
        BirdRouteNudge nudge;
        nudge.bird = bird;
        nudge.found = bestCost < 1e8f;
        nudge.across = bestAcross;
        nudge.above = bestAbove;
        report.nudges.push_back(nudge);
    }

    // "Last Light": the film's own kettle over Taipei in the opening, every bird, every tenth of a second.
    if (act == 2u) {
        for (float t = 0.f; t < float(kLastLightEnd); t += 0.1f) {
            auto frame = workoutFlockFrame(act, t, 1.f, false);
            frame.lean = kLastLightKettleLean;
            for (uint32_t w = 0; w < kLastLightKites; ++w)
                test(workoutBirdPose(act, FlockFormation::Kettle, w, kLastLightKites, float(kLastLightKites), t, 1.f, false, frame),
                     std::string("film kettle bird ") + std::to_string(w), t, false);
        }
    }

    // The workout flock: every formation at three kettles' worth, every 13th bird, two minutes.
    static constexpr const char* kNames[] = {"cluster", "vee", "echelon", "column", "front", "kettle"};
    const uint32_t count = 3u * kFlockUnit;
    for (uint32_t f = 0; f < kFlockFormationCount; ++f) {
        const auto formation = static_cast<FlockFormation>(f);
        for (float t = 0.f; t < 120.f; t += 1.0f) {
            const auto frame = workoutFlockFrame(act, t, 1.f, false);
            for (uint32_t w = 0; w < count; w += 13u)
                test(workoutBirdPose(act, formation, w, count, float(count), t, 1.f, false, frame),
                     std::string("workout ") + kNames[f] + " bird " + std::to_string(w), t, false);
        }
    }

    for (auto* s : statics) { scene->removeActor(*s); s->release(); }
    scene->release();
    material->release();
    report.ran = true;
    return report;
#endif
}

} // namespace boo
