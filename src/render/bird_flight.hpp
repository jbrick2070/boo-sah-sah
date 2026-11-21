#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace boo {

inline constexpr uint32_t kCityBirdCount = 5;

// GPU workout dial (0..10). Level 0 is the authored film: only the authored birds fly. Higher
// levels add a workout flock that gathers around them. The authored birds always keep their
// exact places and lead; every extra bird is a full articulated, ray-traced instance set.
inline constexpr uint32_t kMaxGpuWorkoutLevel = 10;
// One flock unit: a full kettle, and the size at which the extended cluster has closed its circuit.
// A larger flock is more of the same (more skeins, more kettles, a broader cluster), so a flock of
// up to one unit looks the same however high the ceiling is.
inline constexpr uint32_t kFlockUnit = 4000;
inline constexpr uint32_t kMaxWorkoutBirds = 200000;
// What each dial level asks for. The top level asks for everything; the frame-rate floor
// (boo/workout_governor.hpp) decides how many of them this machine can actually fly.
inline constexpr uint32_t workoutBirdCount(uint32_t level) noexcept {
    constexpr std::array<uint32_t, kMaxGpuWorkoutLevel + 1u> birds{
        0u, 40u, 160u, 400u, 1000u, 2000u, kFlockUnit, 8000u, 16000u, 32000u, kMaxWorkoutBirds};
    return birds[level > kMaxGpuWorkoutLevel ? kMaxGpuWorkoutLevel : level];
}

// A bird the dial has just released grows in over a moment instead of appearing whole. Birds
// below `grown` are full size; the arrival front sweeps through the rest, and a bird is full
// size once the front is `band` birds past it.
inline float workoutBirdArrival(uint32_t w, uint32_t grown, float front, float band) noexcept {
    if (w < grown) return 1.f;
    const float u = std::clamp((front - float(w)) / std::max(band, 1.f), 0.f, 1.f);
    return .04f + .96f * u * u * (3.f - 2.f * u);
}

// How the workout flock flies. These are formation types from the ornithological literature, not
// drawn shapes: Heppner's line and cluster formations (F. H. Heppner, "Avian flight formations",
// Bird-Banding 45, 1974) and the thermal-soaring kettle of raptors. Each is flown by a species
// that flies it in life, and a bigger flock extends the way real ones do: more skeins, a longer
// cluster, a taller kettle. Everything is a closed-form function of film time.
enum class FlockFormation : uint32_t {
    ExtendedCluster = 0, // loose flock, longer than wide, following the film's own birds
    Vee,                 // line formation: skeins in vee and J, leader at the apex
    Echelon,             // line formation: one staggered diagonal
    Column,              // line formation: line astern
    Front,               // line formation: line abreast
    Kettle,              // raptors circling up a thermal, gliding out at the top and rejoining below
    Count
};
inline constexpr uint32_t kFlockFormationCount = static_cast<uint32_t>(FlockFormation::Count);

enum class BirdSpecies : uint32_t { BlackKite = 0, GreatEgret = 1 }; // Milvus migrans, Ardea alba

// Black kites soar and kettle but never fly in lines; great egrets commute to roost in vees, echelons
// and lines. A loose cluster is whichever species the film is showing.
inline constexpr BirdSpecies flockSpecies(FlockFormation formation, bool filmShowsKites) noexcept {
    return formation == FlockFormation::Kettle ? BirdSpecies::BlackKite
         : formation == FlockFormation::ExtendedCluster ? (filmShowsKites ? BirdSpecies::BlackKite : BirdSpecies::GreatEgret)
         : BirdSpecies::GreatEgret;
}
inline constexpr bool isLineFormation(FlockFormation formation) noexcept {
    return formation == FlockFormation::Vee || formation == FlockFormation::Echelon ||
           formation == FlockFormation::Column || formation == FlockFormation::Front;
}

inline float birdHash01(uint32_t bird, uint32_t salt) noexcept {
    uint32_t x = bird * 0x9E3779B9u ^ (salt * 0x85EBCA6Bu);
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return float(x & 0xFFFFFFu) / 16777215.f;
}

struct BirdFlightPose {
    std::array<float, 3> center;
    float yaw, pitch, roll, scale;
    float wingLift, wingFlex; // Flex is relative to the inner wing, in radians.
};

// The closed circuit the film's birds fly in each city (world coordinates).
struct BirdRoute { float anchor, speed, x, y, z, xs, xc, ys, yc, zs, zc; };
inline constexpr BirdRoute birdRoute(uint32_t kit, bool trilogy) noexcept {
    return kit == 2 ? BirdRoute{trilogy ? 147.f : 12.f, .18f, -2,22,34, 15,1,-2,6,10,12}
         : kit == 1 ? BirdRoute{trilogy ? 84.f : 257.f, .20f, 3,13.2f,9.5f, -15,0,1.3f,1.65f,8,8.5f}
                    : BirdRoute{42.f, .19f, -2,13.5f,35, 19,0,-2,2,11,9};
}

// A follower's place on the leaders' path: seconds of route time behind them, and its own
// offset across and above the curve.
struct WorkoutSlot { float delay, lateral, height, size; };

// See kTaipeiFlockNudge.
// Measured smallest moves (1.25 m and 3.5 m across, none up), plus a quarter metre to spare. Both
// birds stay at least 3 m from every other bird of the flock.
inline constexpr float kTaipeiNudge1Across = 1.5f, kTaipeiNudge1Above = 0.f;
inline constexpr float kTaipeiNudge3Across = 3.75f, kTaipeiNudge3Above = 0.f;

// Alternate short bouts of flapping with broad glides. During recovery
// the primaries trail the upper wing, then open out on the downstroke.
inline void birdWingStroke(bool kite, float t, float individual, float motion,
                           float& wingLift, float& wingFlex) noexcept {
    const float bout = std::sin(t*(kite ? .82f : .56f) - individual*1.15f);
    const float blend = std::clamp((bout + (kite ? .08f : .78f)) * 2.f, 0.f, 1.f);
    const float flap = motion*blend*blend*(3.f-2.f*blend);
    const float stroke = t*(kite ? 14.4f : 11.2f)*(1.f+individual*.017f)
                       - individual*1.77f + .27f*std::sin(.93f*t+individual);
    wingLift = .10f + flap*(.12f + .72f*std::sin(stroke));
    wingFlex = -.08f - flap*(.26f + .32f*std::cos(stroke));
}

// A bird's place in the film's flock: metres behind the leader along the curve, across it, and
// above it, and its size.
struct FlockMember { float trail, lateral, height, size; };
// Closed circuits in world coordinates. Offsets along and across the curve
// let the outer birds take a wider bank without snapping into formation.
inline constexpr std::array<FlockMember, kCityBirdCount> kCityFlock{{
    { 0.f,   0.f,   0.f, 1.10f},
    {-1.75f,-2.0f, .85f, 0.99f},
    {-3.35f, 2.35f,-.40f, 1.06f},
    {-4.65f,-4.20f,-.55f, 0.95f},
    {-6.00f, 4.65f, .20f, 1.02f}
}};
// Over Taipei, the two kites on the left flank pass a building west of the terrace as the flock
// settles over the rooftops (38-42 s, and 172-177 s on the trilogy route). These nudges, found by
// the PhysX bird-route sweep's search for the smallest move, keep each of them at least
// kBirdBuildingClearance from it: {across, above} in metres, added to its place in kCityFlock.
inline constexpr std::array<std::array<float, 2>, kCityBirdCount> kTaipeiFlockNudge{{
    {0.f, 0.f}, {kTaipeiNudge1Across, kTaipeiNudge1Above}, {0.f, 0.f}, {kTaipeiNudge3Across, kTaipeiNudge3Above}, {0.f, 0.f}
}};

inline BirdFlightPose birdFlightPoseOf(uint32_t kit, const FlockMember& member, float individual, float filmTime,
                                       float motionScale, bool trilogy, float delay) noexcept {
    const bool kite = kit == 2;
    const BirdRoute route = birdRoute(kit, trilogy);
    const float motion = std::clamp(motionScale, 0.f, 1.f);
    // The delay is in route time, so a frozen (reduced-motion) flock still holds its formation.
    const float t = (filmTime - route.anchor) * motion - delay;
    const float phase = t * route.speed;
    const float s = std::sin(phase), c = std::cos(phase);
    const float xc = route.xc, zc = route.zc;
    BirdFlightPose pose{};
    pose.center = {route.x + route.xs*s + xc*c,
                   route.y + route.ys*s + route.yc*c + member.height,
                   route.z + route.zs*s + zc*c};
    float vx = route.speed * (route.xs*c - xc*s);
    float vy = route.speed * (route.ys*c - route.yc*s);
    float vz = route.speed * (route.zs*c - zc*s);
    const float ax = -route.speed*route.speed*(route.xs*s + xc*c);
    const float az = -route.speed*route.speed*(route.zs*s + zc*c);
    const float speed = std::hypot(vx,vz);
    const float yawRate = (ax*vz - az*vx)/(speed*speed);
    const float tx = vx/speed, tz = vz/speed;
    // Transport the formation with the tangent to keep wings separated at
    // the tight end of each turn. Followers fly their own tangential heading.
    const float offsetX = member.trail*tx + member.lateral*tz;
    const float offsetZ = member.trail*tz - member.lateral*tx;
    pose.center[0] += offsetX; vx += yawRate*offsetZ;
    pose.center[2] += offsetZ; vz -= yawRate*offsetX;
    if (kite) {
        // Leave the observatory's high thermals and settle over the far
        // rooftops, where the birds can still be seen beyond the sanctuary.
        const float u = std::clamp((route.anchor+t-(trilogy ? 151.f : 27.f))/18.f, 0.f, 1.f);
        const float blend = u*u*u*(u*(6.f*u-15.f)+10.f);
        const float rate = (30.f/18.f)*u*u*(1.f-u)*(1.f-u);
        pose.center[0] -= 2.f*blend;   vx -= 2.f*rate;
        pose.center[1] -= 6.f*blend; vy -= 6.f*rate;
        pose.center[2] += 12.f*blend; vz += 12.f*rate;
    }
    pose.yaw = std::atan2(vx,vz);
    pose.pitch = -std::atan2(vy,std::hypot(vx,vz));
    // A coordinated turn leans toward the centre of curvature. Kites trim
    // their wings in the thermal. Egrets hold a gentler bank over the water.
    const float inward = yawRate * std::hypot(vx,vz);
    pose.roll = -std::atan2(inward, 9.81f) +
        motion*(kite ? .23f : .09f)*std::sin(2.f*phase + member.lateral*.13f);
    pose.scale = member.size;
    birdWingStroke(kite, t, individual, motion, pose.wingLift, pose.wingFlex);
    return pose;
}

// The film's own birds keep their places in kCityFlock (nudged clear of buildings over Taipei). A
// workout bird flies the same path later.
inline FlockMember cityFlockMember(uint32_t kit, uint32_t bird) noexcept {
    FlockMember member = kCityFlock[bird % kCityBirdCount];
    if (kit == 2) {
        member.lateral += kTaipeiFlockNudge[bird % kCityBirdCount][0];
        member.height += kTaipeiFlockNudge[bird % kCityBirdCount][1];
    }
    return member;
}

inline BirdFlightPose birdFlightPose(uint32_t kit, uint32_t bird, float filmTime,
                                     float motionScale, bool trilogy,
                                     const WorkoutSlot* slot = nullptr) noexcept {
    if (slot)
        return birdFlightPoseOf(kit, FlockMember{0.f, slot->lateral, slot->height, slot->size}, 5.f * birdHash01(bird, 5u),
                                filmTime, motionScale, trilogy, slot->delay);
    return birdFlightPoseOf(kit, cityFlockMember(kit, bird), float(bird % kCityBirdCount), filmTime, motionScale, trilogy, 0.f);
}

// Shared per frame by every bird of the workout flock.
// Above the tallest building of each city, in world metres: Shenzhen's reaches 93.2 m and Taipei's
// 80.4 m (measured by the PhysX bird-route sweep, --physx-route-test). Birds beyond the first flock
// unit fly above this, as a great flock over a roost does, so none passes through a building.
// No bird may come closer than this to any building (metres; about 16 feet). The PhysX bird-route
// sweep enforces it.
inline constexpr float kBirdBuildingClearance = 5.f;

inline constexpr float cityRoofline(uint32_t kit) noexcept {
    return kit == 1u ? 96.f : kit == 2u ? 84.f : -1e9f;
}

// How much higher each workout flock flies than its natural height in each city, so that every bird
// keeps kBirdBuildingClearance from every building. Measured by the PhysX bird-route sweep
// (--physx-route-test, 2026-11-21) with 2 m added: the lift it reported for the first skein, the
// cluster and the first kettle. Zero where it found nothing to clear.
struct FlockLift { float firstSkein, cluster, firstKettle; };
inline constexpr FlockLift cityFlockLift(uint32_t kit) noexcept {
    return kit == 1u ? FlockLift{17.2f, 0.f, 0.f}       // Shenzhen: skein 15.2 m
         : kit == 2u ? FlockLift{12.8f, 15.3f, 17.2f}   // Taipei: skein 10.8, cluster 13.3, kettle 15.2 m
                     : FlockLift{0.f, 0.f, 0.f};
}

struct WorkoutFlockFrame {
    std::array<float, 3> centre{}; // centre of the film's bird circuit in this city, kites' drift included
    float time = 0.f;              // film time scaled by the motion setting
    float motion = 1.f;
    float roofline = -1e9f;        // cityRoofline of this city
    FlockLift lift{0.f, 0.f, 0.f}; // cityFlockLift of this city
};

inline WorkoutFlockFrame workoutFlockFrame(uint32_t kit, float filmTime, float motionScale, bool trilogy) noexcept {
    const BirdRoute route = birdRoute(kit, trilogy);
    WorkoutFlockFrame frame;
    frame.motion = std::clamp(motionScale, 0.f, 1.f);
    frame.time = filmTime * frame.motion;
    frame.centre = {route.x, route.y, route.z};
    frame.roofline = cityRoofline(kit);
    frame.lift = cityFlockLift(kit);
    if (kit == 2) {
        // The kites' circuit settles over the far rooftops; the flock's airspace moves with it.
        const float t = (filmTime - route.anchor) * frame.motion;
        const float u = std::clamp((route.anchor + t - (trilogy ? 151.f : 27.f)) / 18.f, 0.f, 1.f);
        const float blend = u*u*u*(u*(6.f*u-15.f)+10.f);
        frame.centre[0] -= 2.f*blend; frame.centre[1] -= 6.f*blend; frame.centre[2] += 12.f*blend;
    }
    return frame;
}

namespace bird_detail {
inline constexpr float kTau = 6.2831853f;
// Flight speed at the film's tempo: its own birds cover about three metres a second.
inline constexpr float kCruise = 3.4f;
// The model's wingspan is 1.66 m. Birds in line formations hold station about one wingspan
// apart across the line, wingtips close to the upwash of the bird ahead.
inline constexpr float kWingtipStation = 1.62f;

// Skeins come in assorted sizes, as they do in life: 15, 9, 21, 7, 13, 11, 17, 5, 19, 12, repeating.
struct SkeinPlace { uint32_t skein, place, size, first; }; // first: index of the skein's leader
inline SkeinPlace skeinPlace(uint32_t w, uint32_t count) noexcept {
    constexpr std::array<uint32_t, 10> sizes{15u, 9u, 21u, 7u, 13u, 11u, 17u, 5u, 19u, 12u};
    constexpr uint32_t period = 129u;
    const uint32_t cycle = w / period;
    uint32_t rest = w % period, k = 0u, first = cycle * period;
    while (rest >= sizes[k]) { rest -= sizes[k]; first += sizes[k]; ++k; }
    return {cycle * 10u + k, rest, std::min(sizes[k], count - first), first}; // the last skein may be short
}

// Great egrets in a line formation on a broad commuting circuit above the city.
inline BirdFlightPose linePose(FlockFormation formation, uint32_t w, uint32_t count, uint32_t id,
                               const WorkoutFlockFrame& frame) noexcept {
    const SkeinPlace at = skeinPlace(w, count);
    const uint32_t g = at.skein + 1000u; // hash stream for the skein, apart from the birds'
    // Each skein keeps its own lane and height. The first flies lowest and nearest, for the camera.
    // Past one flock unit the later skeins take wider and higher lanes, so the sky fills outward
    // and no skein already flying has to move.
    const float reach = at.first < kFlockUnit ? 1.f : std::sqrt(float(at.first) / float(kFlockUnit));
    const float lane = at.skein == 0u ? 1.f : .8f + .9f * reach * birdHash01(g, 1u);
    const float a = 76.f * lane, b = 60.f * lane; // broad enough that a skein stays nearly straight
    float altitude = frame.centre[1] + (at.skein == 0u ? 16.f + frame.lift.firstSkein
        : 16.f + 24.f * (.5f + .5f * reach) * birdHash01(g, 2u)); // clear of the towers
    // Skeins beyond the first flock unit commute above the roofline, on lanes wide enough to meet the
    // tallest towers.
    // Staggered skeins: under the roofline only the first skein flies (the camera's, low beside the
    // film's own birds, on a lane clear of the towers). Every other skein has its own altitude band,
    // 8 m apart, stacked above the roofline, so skeins never share air and none meets a building.
    if (at.skein > 0u && frame.roofline > -1e8f)
        altitude = frame.roofline + kBirdBuildingClearance + 2.f + 8.f * float((at.skein - 1u) % 8u);
    const float speed = kCruise * (.92f + .16f * birdHash01(g, 5u));
    const float omega = speed / (68.f * lane);
    const float psiLead = kTau * birdHash01(g, 3u) + omega * frame.time;

    // Station in the formation: metres behind the leader, metres to its right, metres above.
    // Place 0 is the leader; later places fill the arms outward.
    float trail = 0.f, lateral = 0.f, lift = 0.f;
    if (at.place > 0u) {
        const float halfAngle = .56f + .24f * birdHash01(g, 6u); // 32 to 46 degrees off the flight line
        const float depth = kWingtipStation / std::tan(halfAngle);
        uint32_t rank = at.place;
        float side = (birdHash01(g, 7u) < .5f) ? -1.f : 1.f;
        if (formation == FlockFormation::Vee) {
            // Arms are rarely equal: most vees are lopsided, and a very lopsided vee is a J.
            const float share = .5f + (birdHash01(g, 8u) - .5f) * .7f;
            const uint32_t nearArm = static_cast<uint32_t>(std::lround(float(at.size - 1u) * share));
            if (at.place > nearArm) { rank = at.place - nearArm; side = -side; }
        }
        if (formation == FlockFormation::Column) {
            trail = 2.6f * float(rank);   // line astern, each bird a little above the wake ahead
            lift = .10f * float(rank);
        } else if (formation == FlockFormation::Front) {
            rank = (at.place + 1u) / 2u;  // line abreast, filling outward on both sides
            side = (at.place & 1u) ? 1.f : -1.f;
            lateral = side * 1.75f * float(rank);
            trail = .05f * float(rank) * float(rank); // the ends of a front hang back a little
        } else {
            trail = depth * float(rank);
            lateral = side * kWingtipStation * float(rank);
        }
        // Station keeping is never perfect: each bird drifts a little about its place.
        const float wander = frame.time * (.31f + .2f * birdHash01(id, 6u)) + kTau * birdHash01(id, 7u);
        trail += .16f * std::sin(wander);
        lateral += .12f * std::sin(1.7f * wander + 1.f);
        lift += .14f * std::sin(1.3f * wander + 2.f);
    }

    // A bird trail metres behind the leader is where the leader was that long ago.
    const float pathSpeedLead = omega * std::hypot(a * std::sin(psiLead), b * std::cos(psiLead));
    const float lag = trail / std::max(pathSpeedLead, .2f);
    const float psi = psiLead - omega * lag;
    const float sn = std::sin(psi), cs = std::cos(psi);
    const float tx = -a * sn, tz = b * cs;              // direction of travel, unnormalised
    const float along = std::hypot(tx, tz);
    const float fx = tx / along, fz = tz / along;
    BirdFlightPose pose{};
    pose.center = {frame.centre[0] + a * cs + lateral * fz,
                   altitude + lift,
                   frame.centre[2] + b * sn - lateral * fx};
    pose.yaw = std::atan2(fx, fz);
    pose.pitch = 0.f;
    // Bank for the turn the circuit asks of it, with the small corrections of station keeping.
    const float curvature = a * b / (along * along * along);
    pose.roll = std::atan2(speed * speed * curvature, 9.81f) +
                frame.motion * .05f * std::sin(.9f * frame.time + float(at.place));
    pose.scale = .94f + .14f * birdHash01(id, 4u);
    // Every bird in the skein beats to the leader's rhythm, delayed by its distance astern, so
    // each wingtip follows the path of the one ahead (Portugal et al., Nature 505, 2014).
    birdWingStroke(false, frame.time - lag, 5.f * birdHash01(g, 9u), frame.motion, pose.wingLift, pose.wingFlex);
    return pose;
}

// Where a thermal rises. The first is over the film's birds. A big movement of raptors fills
// thermal after thermal, so each further kettle forms a little farther out, spaced like the seeds
// of a sunflower so that no two share air, and higher above the roofs.
inline std::array<float, 3> thermalBase(uint32_t thermal, const WorkoutFlockFrame& frame) noexcept {
    if (thermal == 0u) return {frame.centre[0], frame.centre[1] - 1.f + frame.lift.firstKettle, frame.centre[2]};
    const float out = 80.f * std::sqrt(float(thermal));
    const float bearing = 2.39996323f * float(thermal) + .9f;
    // Further thermals rise from the warm roofs, so their kettles begin above the roofline.
    const float above = std::max(frame.centre[1] + 18.f, frame.roofline + 4.f);
    return {frame.centre[0] + out * std::cos(bearing),
            above + 16.f * birdHash01(thermal + 7000u, 1u),
            frame.centre[2] + out * std::sin(bearing)};
}

// Position of a kettling kite and how hard it is banking (0 gliding wings level .. 1 circling).
// `flockSize` is the size of the whole flock; bird w circles in thermal w / kFlockUnit.
inline std::array<float, 3> kettlePosition(float flockSize, uint32_t w, const WorkoutFlockFrame& frame,
                                           float time, float& circling) noexcept {
    const uint32_t id = kCityBirdCount + w;
    const uint32_t thermal = w / kFlockUnit;
    const float inThermal = std::clamp(flockSize - float(thermal * kFlockUnit), 0.f, float(kFlockUnit));
    const float fill = std::sqrt(inThermal / float(kFlockUnit));
    const float top = 14.f + 40.f * fill;               // the column grows taller as more birds join
    const float core = 7.f + 9.f * birdHash01(id, 1u);  // each bird's own circle in the thermal
    const float omega = kCruise / core * (.9f + .2f * birdHash01(id, 2u));
    constexpr float climbShare = .78f;                  // of each cycle spent climbing
    const float climbRate = .55f + .2f * birdHash01(id, 3u);
    const float cycle = top / climbRate / climbShare;
    const float u = time / cycle + birdHash01(id, 5u);
    const float phase = u - std::floor(u);
    float height, glide;
    if (phase < climbShare) {
        height = top * phase / climbShare;
        glide = 0.f;
    } else {
        // Out of the top in a fast, sinking glide round the outside, and back in at the base.
        const float v = (phase - climbShare) / (1.f - climbShare);
        height = top * (1.f - v * v * (3.f - 2.f * v));
        const float bump = std::sin(3.14159265f * v);
        glide = bump * bump;
    }
    circling = 1.f - glide;
    const float radius = core + .10f * height + glide * core * (.6f + .5f * fill); // thermals widen with height
    const float theta = kTau * birdHash01(id, 6u) + omega * time;
    // The column leans downwind: the same wind for every thermal.
    const auto base = thermalBase(thermal, frame);
    return {base[0] + .18f * height + radius * std::cos(theta),
            base[1] + height,
            base[2] + .07f * height + radius * std::sin(theta)};
}

inline BirdFlightPose kettlePose(float flockSize, uint32_t w, const WorkoutFlockFrame& frame) noexcept {
    constexpr float kStep = 1.f / 30.f;
    const uint32_t id = kCityBirdCount + w;
    float circling = 1.f, unused = 1.f;
    const auto here = kettlePosition(flockSize, w, frame, frame.time, circling);
    const auto soon = kettlePosition(flockSize, w, frame, frame.time + kStep, unused);
    BirdFlightPose pose{};
    pose.center = here;
    const float vx = soon[0] - here[0], vy = soon[1] - here[1], vz = soon[2] - here[2];
    const float ground = std::hypot(vx, vz);
    pose.yaw = std::atan2(vx, vz);
    pose.pitch = -std::clamp(std::atan2(vy, std::max(ground, 1e-4f)), -.45f, .45f);
    // Kites circle a thermal at a steady twenty-odd degrees of bank and level out to glide.
    // They all turn the same way here: anticlockwise from above, which leans them positive.
    pose.roll = circling * (.34f + .12f * birdHash01(id, 2u));
    pose.scale = .88f + .24f * birdHash01(id, 4u);
    // Soaring: wings held out, no flapping.
    birdWingStroke(true, frame.time, 0.f, 0.f, pose.wingLift, pose.wingFlex);
    return pose;
}
} // namespace bird_detail

// One bird of the workout flock (w in [0, count)). `count` is the number of birds flying this
// frame; `flockSize` is the same number as it changes smoothly while the dial releases birds, so
// that a growing kettle or cluster swells instead of jumping. Pass float(count) for a steady flock.
inline BirdFlightPose workoutBirdPose(uint32_t kit, FlockFormation formation, uint32_t w, uint32_t count,
                                      float flockSize, float filmTime, float motionScale, bool trilogy,
                                      const WorkoutFlockFrame& frame) noexcept {
    const uint32_t id = kCityBirdCount + w; // stable identity: size, timing and place in the flock
    if (formation == FlockFormation::Kettle) return bird_detail::kettlePose(flockSize, w, frame);
    if (isLineFormation(formation)) return bird_detail::linePose(formation, w, count, id, frame);

    // Extended cluster: the film's birds lead and the rest follow their path in a loose flock,
    // longer than it is wide, each bird at its own distance behind, beside and above.
    const float fill = std::sqrt(std::min(flockSize, float(kFlockUnit)) / float(kFlockUnit));
    const BirdRoute route = birdRoute(kit, trilogy);
    const float lap = bird_detail::kTau / route.speed; // route seconds per circuit
    WorkoutSlot slot;
    slot.delay = 2.4f + lap * std::min(.94f, .10f + .84f * fill) * birdHash01(id, 1u);
    slot.lateral = (birdHash01(id, 2u) - .5f) * (4.f + 7.f * fill);
    slot.height = (birdHash01(id, 3u) - .35f) * (2.5f + 4.5f * fill);
    slot.size = .82f + .32f * birdHash01(id, 4u);
    slot.height += frame.lift.cluster; // above the roofs the film's birds skim, by the clearance
    if (w >= kFlockUnit) {
        // The circuit is full by now. Later birds wheel wider and higher round the outside of
        // it, as a great flock does over a roost, and the birds already flying keep their places.
        const float reach = std::sqrt(float(w) / float(kFlockUnit));
        // Positive lateral is to the right of travel; which side is the outside depends on the
        // way the circuit turns.
        const float outside = (route.xs * route.zc - route.xc * route.zs) > 0.f ? -1.f : 1.f;
        const float outward = outside * slot.lateral; // -5.5 inside .. 5.5 outside
        slot.lateral = outside * (-5.5f + (outward + 5.5f) * (1.f + 1.4f * (reach - 1.f)));
        slot.height = -2.45f + (slot.height + 2.45f) * (1.f + .6f * (reach - 1.f));
        // And above the roofs: the outer birds ride high over the circuit rather than through the towers.
        slot.height = std::max(slot.height, frame.roofline - route.y + 4.f + 8.f * birdHash01(id, 3u));
    }
    return birdFlightPose(kit, id, filmTime, motionScale, trilogy, &slot);
}

// Birds the camera should frame: the first skein of a line formation, the first kettle, otherwise
// the whole flock.
inline constexpr uint32_t flockFocusCount(FlockFormation formation, uint32_t workoutBirds) noexcept {
    return isLineFormation(formation) ? (workoutBirds < 15u ? workoutBirds : 15u)
         : formation == FlockFormation::Kettle ? (workoutBirds < kFlockUnit ? workoutBirds : kFlockUnit)
         : workoutBirds;
}

} // namespace boo
