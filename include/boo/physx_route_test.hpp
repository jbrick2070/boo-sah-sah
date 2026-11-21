#pragma once

// The bird-route sweep (docs/physx_suite_plan.md, step 2). PhysX never flies a bird; instead this
// test sweeps every bird the film can show through the city's buildings as PhysX overlap queries and
// reports each place a bird would pass through a building. Fixes go into the flight data
// (src/render/bird_flight.hpp), never into the flight.

#include "boo/api.hpp"
#include "boo/physx_suite.hpp"

#include <map>
#include <string>
#include <vector>

namespace boo {

struct BirdRouteHit {
    std::string who;      // "authored bird 3", "workout kettle bird 4100", ...
    uint32_t act = 0;     // 1 Shenzhen, 2 Taipei
    float filmTime = 0;   // seconds
    float position[3]{};
};

// The smallest move (metres across the curve and up) that keeps one of the film's birds clear.
struct BirdRouteNudge {
    uint32_t bird = 0;
    bool found = false;
    float across = 0.0f, above = 0.0f;
};

struct BirdRouteReport {
    bool ran = false;
    std::string reason;   // why it did not run
    uint32_t buildings = 0;
    float tallest = 0.0f;  // highest building top, metres
    float tallestAt[2]{};  // its x, z
    uint64_t samples = 0;
    uint32_t authoredHits = 0;
    uint32_t workoutHits = 0;
    std::vector<BirdRouteHit> examples; // the first few of each kind
    std::map<std::string, uint32_t> hitsByFlock; // "workout vee", "authored", ...
    std::map<std::string, float> liftByFlock;    // metres that flock must rise to keep its clearance everywhere
    float authoredClosest = 1e9f;                // closest the film's own birds come to a building, metres
    std::vector<BirdRouteNudge> nudges;          // for each film bird that strays, the smallest move that clears it
};

// Sweep the authored birds over a full circuit, and the workout flock in every formation, through
// `buildings` of city `act`. Uses a CPU scene on the runtime (which must be Ready).
BirdRouteReport sweepBirdRoutes(PhysxSuite& runtime, const std::vector<CityCollisionBox>& buildings, uint32_t act);

} // namespace boo
