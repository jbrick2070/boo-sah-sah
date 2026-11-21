#pragma once

// The workout flock's flight formations (src/render/bird_flight.hpp): each must stay the formation
// its name says, in the species that flies it, and move without jumps.

#include "../../src/render/bird_flight.hpp"

#include <cmath>

namespace {

bool testFlockFormations() {
    using namespace boo;
    // The dial: off is the authored film, each level asks for more, and the top asks for everything
    // (the frame-rate floor decides how much of that a machine gets).
    BOO_CHECK(workoutBirdCount(0) == 0u && workoutBirdCount(1) == 40u && workoutBirdCount(6) == kFlockUnit);
    BOO_CHECK(workoutBirdCount(10) == kMaxWorkoutBirds && workoutBirdCount(99) == kMaxWorkoutBirds);
    for (uint32_t level = 1; level <= kMaxGpuWorkoutLevel; ++level)
        BOO_CHECK(workoutBirdCount(level) > workoutBirdCount(level - 1u));

    // A bird the dial has just released grows in as the arrival front passes it.
    BOO_CHECK(workoutBirdArrival(10u, 20u, 0.f, 8.f) == 1.f);          // already grown
    BOO_CHECK(workoutBirdArrival(30u, 20u, 30.f, 8.f) < .05f);         // the front has just reached it
    BOO_CHECK(workoutBirdArrival(30u, 20u, 38.f, 8.f) == 1.f);         // the front is a band past it
    BOO_CHECK(workoutBirdArrival(30u, 20u, 34.f, 8.f) > .3f && workoutBirdArrival(30u, 20u, 34.f, 8.f) < .7f);
    BOO_CHECK(workoutBirdArrival(0u, ~0u, 0.f, 1.f) == 1.f);           // no batch arriving: everything is full size

    // Each formation is flown by a species that flies it in life.
    BOO_CHECK(flockSpecies(FlockFormation::Kettle, false) == BirdSpecies::BlackKite);
    for (auto line : {FlockFormation::Vee, FlockFormation::Echelon, FlockFormation::Column, FlockFormation::Front}) {
        BOO_CHECK(isLineFormation(line));
        BOO_CHECK(flockSpecies(line, true) == BirdSpecies::GreatEgret);
    }
    BOO_CHECK(flockSpecies(FlockFormation::ExtendedCluster, true) == BirdSpecies::BlackKite);
    BOO_CHECK(flockSpecies(FlockFormation::ExtendedCluster, false) == BirdSpecies::GreatEgret);
    BOO_CHECK(!isLineFormation(FlockFormation::Kettle) && !isLineFormation(FlockFormation::ExtendedCluster));

    // Skeins: every bird has exactly one place, sizes vary, and the last skein may be short.
    {
        const uint32_t count = 160u;
        uint32_t members = 0u, skeins = 0u, lastSkein = ~0u;
        for (uint32_t w = 0; w < count; ++w) {
            const auto at = bird_detail::skeinPlace(w, count);
            if (at.skein != lastSkein) { BOO_CHECK(at.place == 0u); lastSkein = at.skein; ++skeins; }
            BOO_CHECK(at.place < at.size && at.size >= 1u && at.size <= 21u);
            ++members;
        }
        BOO_CHECK(members == count && skeins == 13u); // 129 birds fill ten skeins; 31 more make 15, 9 and a short 7
        BOO_CHECK(bird_detail::skeinPlace(0u, count).size == 15u && bird_detail::skeinPlace(159u, count).size == 7u);
    }

    const uint32_t count = 640u;
    const auto frame = workoutFlockFrame(2u, 150.f, 1.f, false);
    auto ahead = [](const BirdFlightPose& lead, const BirdFlightPose& bird) {
        // Distance of `bird` ahead of `lead` along the leader's heading, and to its right.
        const float dx = bird.center[0] - lead.center[0], dz = bird.center[2] - lead.center[2];
        const float fx = std::sin(lead.yaw), fz = std::cos(lead.yaw);
        return std::array<float, 2>{dx * fx + dz * fz, dx * fz - dz * fx};
    };

    // Vee: the leader is at the apex, both arms trail it, and neighbours hold about a wingspan apart.
    {
        const auto lead = workoutBirdPose(2u, FlockFormation::Vee, 0u, count, float(count), 150.f, 1.f, false, frame);
        bool left = false, right = false;
        for (uint32_t w = 1; w < 15u; ++w) {
            const auto bird = workoutBirdPose(2u, FlockFormation::Vee, w, count, float(count), 150.f, 1.f, false, frame);
            const auto rel = ahead(lead, bird);
            BOO_CHECK(rel[0] < -.3f);                       // behind the leader
            if (std::abs(rel[1]) > .9f) (rel[1] > 0.f ? right : left) = true; // out on one arm or the other
            if (w <= 4u) BOO_CHECK(std::abs(bird.yaw - lead.yaw) < .35f);     // heading the same way
        }
        BOO_CHECK(left && right);
        const auto second = workoutBirdPose(2u, FlockFormation::Vee, 1u, count, float(count), 150.f, 1.f, false, frame);
        const auto third = workoutBirdPose(2u, FlockFormation::Vee, 2u, count, float(count), 150.f, 1.f, false, frame);
        const float station = std::abs(ahead(lead, third)[1] - ahead(lead, second)[1]);
        BOO_CHECK(station > .8f && station < 2.5f);         // about one 1.66 m wingspan
    }
    // Echelon: one diagonal, every bird on the same side.
    {
        const auto lead = workoutBirdPose(2u, FlockFormation::Echelon, 0u, count, float(count), 150.f, 1.f, false, frame);
        const float side = ahead(lead, workoutBirdPose(2u, FlockFormation::Echelon, 1u, count, float(count), 150.f, 1.f, false, frame))[1];
        for (uint32_t w = 1; w < 15u; ++w) {
            const auto rel = ahead(lead, workoutBirdPose(2u, FlockFormation::Echelon, w, count, float(count), 150.f, 1.f, false, frame));
            BOO_CHECK(rel[0] < -.3f && rel[1] * side > 0.f);
        }
    }
    // Column: line astern. Front: line abreast.
    {
        const auto lead = workoutBirdPose(2u, FlockFormation::Column, 0u, count, float(count), 150.f, 1.f, false, frame);
        const auto fifth = workoutBirdPose(2u, FlockFormation::Column, 5u, count, float(count), 150.f, 1.f, false, frame);
        const auto rel = ahead(lead, fifth);
        BOO_CHECK(rel[0] < -10.f && std::abs(rel[1]) < 3.5f);
        const auto frontLead = workoutBirdPose(2u, FlockFormation::Front, 0u, count, float(count), 150.f, 1.f, false, frame);
        const auto wing = workoutBirdPose(2u, FlockFormation::Front, 13u, count, float(count), 150.f, 1.f, false, frame);
        const auto frontRel = ahead(frontLead, wing);
        BOO_CHECK(std::abs(frontRel[1]) > 10.f && std::abs(frontRel[0]) < 5.f);
    }

    // Kettle: black kites soar (no wingbeat), bank into the circle, stay above the base, and the
    // closed climb-and-glide cycle never teleports a bird.
    {
        float highest = 0.f;
        for (uint32_t w = 0; w < 200u; ++w) {
            auto previous = workoutBirdPose(2u, FlockFormation::Kettle, w, count, float(count), 100.f, 1.f, false,
                                            workoutFlockFrame(2u, 100.f, 1.f, false));
            const float glideLift = previous.wingLift;
            for (int step = 1; step <= 400; ++step) {
                const float time = 100.f + .25f * float(step);
                const auto kettleFrame = workoutFlockFrame(2u, time, 1.f, false);
                const auto pose = workoutBirdPose(2u, FlockFormation::Kettle, w, count, float(count), time, 1.f, false, kettleFrame);
                const float moved = std::hypot(pose.center[0] - previous.center[0], pose.center[1] - previous.center[1],
                                               pose.center[2] - previous.center[2]);
                BOO_CHECK(moved < 6.f);                                   // a quarter second of flight
                BOO_CHECK(pose.center[1] >= kettleFrame.centre[1] - 1.5f); // never below the base of the thermal
                BOO_CHECK(pose.wingLift == glideLift);                    // wings held out
                BOO_CHECK(pose.roll >= 0.f && pose.roll < .6f);
                highest = std::max(highest, pose.center[1] - kettleFrame.centre[1]);
                previous = pose;
            }
        }
        BOO_CHECK(highest > 20.f); // the column is tall
    }

    // Extended cluster: every bird trails the film's lead bird along its circuit.
    {
        const auto lead = birdFlightPose(2u, 0u, 150.f, 1.f, false);
        const auto first = workoutBirdPose(2u, FlockFormation::ExtendedCluster, 0u, count, float(count), 150.f, 1.f, false, frame);
        BOO_CHECK(std::hypot(first.center[0] - lead.center[0], first.center[2] - lead.center[2]) < 80.f);
        BOO_CHECK(std::isfinite(first.yaw) && std::isfinite(first.roll) && first.scale > .8f && first.scale < 1.2f);
    }

    // A flock far beyond one unit is more of the same: birds already flying keep their places
    // when more join, and the newcomers take new air (further thermals, wider lanes, the outside
    // of the cluster).
    {
        const uint32_t unit = kFlockUnit, big = 60000u;
        auto same = [](const BirdFlightPose& a, const BirdFlightPose& b) {
            return a.center == b.center && a.yaw == b.yaw && a.roll == b.roll && a.scale == b.scale;
        };
        for (uint32_t f = 0; f < kFlockFormationCount; ++f) {
            const auto formation = static_cast<FlockFormation>(f);
            for (uint32_t w : {0u, 7u, 200u, 3000u}) {
                const auto few = workoutBirdPose(2u, formation, w, unit, float(unit), 150.f, 1.f, false, frame);
                const auto many = workoutBirdPose(2u, formation, w, big, float(big), 150.f, 1.f, false, frame);
                BOO_CHECK(same(few, many));
            }
            for (uint32_t w = unit; w < big; w += 487u) {
                const auto pose = workoutBirdPose(2u, formation, w, big, float(big), 150.f, 1.f, false, frame);
                for (int axis = 0; axis < 3; ++axis) BOO_CHECK(std::isfinite(pose.center[axis]));
                BOO_CHECK(std::isfinite(pose.yaw) && std::isfinite(pose.pitch) && std::isfinite(pose.roll));
                BOO_CHECK(std::hypot(pose.center[0] - frame.centre[0], pose.center[2] - frame.centre[2]) < 900.f);
            }
        }
        // Each further kettle has its own thermal, clear of every other.
        for (uint32_t a = 0; a < 25u; ++a) {
            const auto baseA = bird_detail::thermalBase(a, frame);
            for (uint32_t b = a + 1u; b < 25u; ++b) {
                const auto baseB = bird_detail::thermalBase(b, frame);
                BOO_CHECK(std::hypot(baseA[0] - baseB[0], baseA[2] - baseB[2]) > 70.f);
            }
        }
        // A kite of the second kettle circles the second thermal, not the first.
        const auto second = bird_detail::thermalBase(1u, frame);
        for (uint32_t w = unit; w < unit + 60u; ++w) {
            const auto pose = workoutBirdPose(2u, FlockFormation::Kettle, w, big, float(big), 150.f, 1.f, false, frame);
            BOO_CHECK(std::hypot(pose.center[0] - second[0], pose.center[2] - second[2]) < 60.f);
        }
        // A kettle that is still filling swells smoothly: a few more birds barely move the others.
        const auto before = workoutBirdPose(2u, FlockFormation::Kettle, 50u, 2000u, 2000.f, 150.f, 1.f, false, frame);
        const auto after = workoutBirdPose(2u, FlockFormation::Kettle, 50u, 2010u, 2010.f, 150.f, 1.f, false, frame);
        BOO_CHECK(std::hypot(before.center[0] - after.center[0], before.center[1] - after.center[1],
                             before.center[2] - after.center[2]) < .5f);
        // Past one unit the cluster grows outward: later birds fly farther from the circuit's centre.
        const BirdRoute route = birdRoute(2u, false);
        double inner = 0.0, outer = 0.0;
        for (uint32_t i = 0; i < 400u; ++i) {
            const auto near = workoutBirdPose(2u, FlockFormation::ExtendedCluster, 100u + i, big, float(big), 150.f, 1.f, false, frame);
            const auto far = workoutBirdPose(2u, FlockFormation::ExtendedCluster, 50000u + i, big, float(big), 150.f, 1.f, false, frame);
            inner += std::hypot(near.center[0] - frame.centre[0], near.center[2] - frame.centre[2]);
            outer += std::hypot(far.center[0] - frame.centre[0], far.center[2] - frame.centre[2]);
        }
        (void)route;
        BOO_CHECK(outer > inner * 1.5);
        // The camera frames the first skein or the first kettle of a big flock, not all of it.
        BOO_CHECK(flockFocusCount(FlockFormation::Vee, big) == 15u);
        BOO_CHECK(flockFocusCount(FlockFormation::Kettle, big) == kFlockUnit);
        BOO_CHECK(flockFocusCount(FlockFormation::ExtendedCluster, big) == big);
    }

    // Frozen motion (reduced motion at zero) still holds every formation apart and finite.
    for (uint32_t f = 0; f < kFlockFormationCount; ++f) {
        const auto formation = static_cast<FlockFormation>(f);
        const auto still = workoutFlockFrame(1u, 260.f, 0.f, false);
        const auto a = workoutBirdPose(1u, formation, 3u, count, float(count), 260.f, 0.f, false, still);
        const auto b = workoutBirdPose(1u, formation, 4u, count, float(count), 260.f, 0.f, false, still);
        for (int axis = 0; axis < 3; ++axis) BOO_CHECK(std::isfinite(a.center[axis]) && std::isfinite(b.center[axis]));
        BOO_CHECK(std::isfinite(a.yaw) && std::isfinite(a.pitch) && std::isfinite(a.roll));
        BOO_CHECK(std::hypot(a.center[0] - b.center[0], a.center[1] - b.center[1], a.center[2] - b.center[2]) > .2f);
    }
    return true;
}

} // namespace
