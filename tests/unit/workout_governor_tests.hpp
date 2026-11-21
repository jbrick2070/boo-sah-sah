#pragma once

// The workout dial's frame-rate floor (include/boo/workout_governor.hpp), run against simulated
// machines: whatever the dial asks for, the frame rate must not be taken under the floor.

#include "boo/workout_governor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

// A machine whose frame costs `base` seconds plus `perBird` for each workout bird it draws.
struct SimulatedMachine {
    double base = 0.008;        // seconds for the film alone
    double perBird = 0.6e-6;    // seconds each workout bird adds
    double frameCap = 0.0;      // vertical sync: no frame is shorter than this
    double jitter = 0.0;        // +- share of noise on every frame
    uint32_t seed = 12345u;

    double noise() noexcept {
        seed = seed * 1664525u + 1013904223u;
        return 1.0 + jitter * (double(seed >> 8) / double(1u << 24) * 2.0 - 1.0);
    }
    double gpuSeconds(uint32_t birds) noexcept { return (base + perBird * double(birds)) * noise(); }
    double frameSeconds(uint32_t birds) noexcept { return std::max(frameCap, gpuSeconds(birds)); }
};

struct SimulatedRun {
    uint32_t birds = 0;          // flying at the end
    double slowestFrame = 0.0;   // seconds, over the whole run
    double lastFrame = 0.0;
    bool atFloor = false;
};

// Runs the governor for `seconds` and reports where it ended and the worst frame on the way.
SimulatedRun runGovernor(boo::WorkoutGovernor& governor, SimulatedMachine& machine, uint32_t requested,
                         float floorFps, double seconds, uint32_t limit = 100000u, bool visible = true) {
    SimulatedRun run;
    double clock = 0.0, frame = 0.0, gpu = 0.0;
    while (clock < seconds) {
        boo::WorkoutGovernorInput in;
        in.frameSeconds = frame;
        in.gpuSeconds = gpu;
        in.requested = requested;
        in.limit = limit;
        in.floorFps = floorFps;
        in.flockVisible = visible;
        governor.update(in);
        // A hidden flock costs nothing to draw.
        const uint32_t drawn = visible ? governor.birds() : 0u;
        gpu = machine.gpuSeconds(drawn);
        frame = std::max(machine.frameCap, gpu);
        clock += frame;
        run.slowestFrame = std::max(run.slowestFrame, frame);
        run.lastFrame = frame;
    }
    run.birds = governor.birds();
    run.atFloor = governor.atFloor();
    return run;
}

bool testWorkoutGovernor() {
    using namespace boo;
    const float floor = 24.f;
    const double floorSeconds = 1.0 / 24.0;

    // A fast machine asked for everything: it fills until the frame rate is at its aim of 28,
    // well clear of the floor of 24, and no frame on the way there is slower than the floor.
    {
        WorkoutGovernor governor;
        SimulatedMachine machine; // 125 fps alone, 0.6 microseconds a bird: 28 fps is near 46,000 birds
        const auto run = runGovernor(governor, machine, 100000u, floor, 40.0);
        const double fps = 1.0 / run.lastFrame;
        BOO_CHECK(run.slowestFrame <= floorSeconds);
        BOO_CHECK(fps >= 28.0 && fps < 29.5);
        BOO_CHECK(run.birds > 40000u && run.birds < 46200u);
        BOO_CHECK(run.atFloor);
        BOO_CHECK(governor.steadyBirds() == run.birds && governor.peakBirds() >= run.birds);
        BOO_CHECK(governor.secondsPerBird() > 0.5e-6 && governor.secondsPerBird() < 0.7e-6);
        BOO_CHECK(governor.grown() == run.birds); // everything has finished arriving

        // The dial comes down: the birds go at once, and the floor no longer limits anything.
        const auto fewer = runGovernor(governor, machine, 2000u, floor, 3.0);
        BOO_CHECK(fewer.birds == 2000u && !fewer.atFloor);
        // Back to zero is the authored film.
        const auto none = runGovernor(governor, machine, 0u, floor, 1.0);
        BOO_CHECK(none.birds == 0u && governor.released() == 0u);
    }

    // The same machine with noisy frame times still holds the floor.
    {
        WorkoutGovernor governor;
        SimulatedMachine machine;
        machine.jitter = 0.03;
        const auto run = runGovernor(governor, machine, 100000u, floor, 60.0);
        BOO_CHECK(run.slowestFrame <= floorSeconds);
        BOO_CHECK(run.birds > 34000u && run.birds < 47500u);
    }

    // A request the machine can fly easily is simply released, with nothing held back.
    {
        WorkoutGovernor governor;
        SimulatedMachine machine;
        const auto run = runGovernor(governor, machine, 4000u, floor, 20.0);
        BOO_CHECK(run.birds == 4000u && !run.atFloor);
        BOO_CHECK(run.slowestFrame <= floorSeconds);
    }

    // A slow machine: 33 fps alone and five microseconds a bird. It stops near 1,000 birds
    // however many are asked for.
    {
        WorkoutGovernor governor;
        SimulatedMachine machine;
        machine.base = 0.030;
        machine.perBird = 5e-6;
        const auto run = runGovernor(governor, machine, 32000u, floor, 40.0);
        BOO_CHECK(run.slowestFrame <= floorSeconds);
        BOO_CHECK(run.birds > 800u && run.birds < 1150u && run.atFloor);
    }

    // A machine already under the floor on the film alone gets no workout birds at all, and
    // neither does one that is above the floor but has nothing to spare over the aim.
    for (double base : {0.050, 0.037}) {
        WorkoutGovernor governor;
        SimulatedMachine machine;
        machine.base = base;
        const auto run = runGovernor(governor, machine, 4000u, floor, 10.0);
        BOO_CHECK(run.birds == 0u && run.atFloor);
    }

    // Vertical sync hides the load in the frame time until it is too late; GPU time does not.
    {
        WorkoutGovernor governor;
        SimulatedMachine machine;
        machine.base = 0.005;
        machine.frameCap = 1.0 / 60.0;
        const auto run = runGovernor(governor, machine, 100000u, floor, 40.0);
        BOO_CHECK(run.slowestFrame <= floorSeconds);
        BOO_CHECK(run.birds > 44000u && run.birds < 51200u);
    }

    // The scene gets heavier under a full flock (a new shot): birds are shed within half a
    // second, and the flock settles at what the heavier scene allows.
    {
        WorkoutGovernor governor;
        SimulatedMachine machine;
        runGovernor(governor, machine, 100000u, floor, 40.0);
        machine.base = 0.022;
        const uint32_t before = governor.birds();
        const auto reacted = runGovernor(governor, machine, 100000u, floor, 0.5);
        BOO_CHECK(reacted.birds < before);
        const auto settled = runGovernor(governor, machine, 100000u, floor, 30.0);
        BOO_CHECK(1.0 / settled.lastFrame >= 25.4);
        BOO_CHECK(settled.birds > 15000u && settled.birds < 28900u);
    }

    // With the floor switched off the request is released exactly, whatever it does to the frame rate.
    {
        WorkoutGovernor governor;
        SimulatedMachine machine;
        const auto run = runGovernor(governor, machine, 100000u, 0.f, 5.0);
        BOO_CHECK(run.birds == 100000u && !run.atFloor);
        BOO_CHECK(run.lastFrame > floorSeconds);
    }

    // The renderer's room is a hard limit.
    {
        WorkoutGovernor governor;
        SimulatedMachine machine;
        const auto run = runGovernor(governor, machine, 100000u, floor, 30.0, 8000u);
        BOO_CHECK(run.birds == 8000u && !run.atFloor);
    }

    // While the flock is not being drawn there is nothing to measure, so nothing is released.
    {
        WorkoutGovernor governor;
        SimulatedMachine machine;
        const auto hidden = runGovernor(governor, machine, 100000u, floor, 10.0, 100000u, false);
        BOO_CHECK(hidden.birds == 0u);
    }

    // A batch arrives over a moment: the count climbs, and the smooth size follows it.
    {
        WorkoutGovernor governor;
        WorkoutGovernorInput in;
        in.requested = 1000u; in.limit = 100000u; in.floorFps = 0.f; in.frameSeconds = 1.0 / 60.0;
        governor.update(in);
        BOO_CHECK(governor.releasing() && governor.birds() < 1000u && governor.grown() == 0u);
        uint32_t last = governor.birds();
        float lastSize = governor.flockSize();
        for (int i = 0; i < 120; ++i) {
            governor.update(in);
            BOO_CHECK(governor.birds() >= last && governor.flockSize() >= lastSize);
            BOO_CHECK(governor.flockSize() <= float(governor.released()));
            last = governor.birds();
            lastSize = governor.flockSize();
        }
        BOO_CHECK(!governor.releasing() && governor.birds() == 1000u && governor.grown() == 1000u);
    }
    return true;
}

} // namespace
