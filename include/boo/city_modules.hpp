#pragma once

#include "boo/bmesh.hpp"

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

namespace boo {
struct CityModulePlacement {
    uint32_t kit = 0, part = 0, seed = 0;
    std::array<uint32_t, 3> modules{}; // Near, middle and distant meshes.
    std::array<float, 12> rows{}; // Module to authored kit, row-major affine transform.
    std::array<float, 3> center{}; // Bounding sphere in authored kit coordinates.
    float radius = 0.0f;
    std::array<int32_t, 2> chunk{};
};

struct CityModuleLayout {
    std::vector<Bmesh> modules;
    std::vector<CityModulePlacement> placements;
};

// Convert the two procedural district partitions into placements at their original
// block coordinates. Retain authored structures, hero kits and unrecognized geometry.
void appendCityModules(CityModuleLayout& layout, uint32_t kit, Bmesh& authored);
inline constexpr uint32_t kSkyPlane = 76, kSkyDrone = 77;
Bmesh buildCityAircraft(uint32_t part);
inline constexpr uint32_t kShanghaiPoliceCabin=184, kShanghaiPoliceRotor=185, kShanghaiPoliceFan=186;
inline constexpr std::array<float,3> kShanghaiMainHub{0.f,1.39f,-.28f};
inline constexpr std::array<float,3> kShanghaiTailHub{0.f,.57f,-5.18f};
Bmesh buildShanghaiPoliceHelicopter();
struct ShanghaiPolicePose {
    std::array<float,3> center;
    float yaw, bank;
};
ShanghaiPolicePose shanghaiPolicePose(float riverSeconds, float motionScale) noexcept;
inline constexpr uint32_t kParcelCase=173, kParcelLine=174, kParcelDock=175;
inline constexpr std::array<float,3> kParcelDockPosition{3.35f,0.f,8.53f};
Bmesh buildParcelHardware(uint32_t part);
// "Last Light": the banner's mast (kBannerMast) or its silk (kBannerCloth), include/boo/wind_banner.hpp.
Bmesh buildWindBanner(uint32_t part);
inline constexpr uint32_t kObservatoryLiftShaft=180, kObservatoryLiftCab=181;
inline constexpr uint32_t kWaterfrontLiftRails=182, kWaterfrontLiftCab=183;
Bmesh buildCityLift(uint32_t part);
float cityLiftTravel(bool observatory, float routeSeconds, float motionScale) noexcept;
struct ParcelPose {
    std::array<float,3> center;
    std::array<float,3> lineOrigin;
    float lineLength, yaw;
};
ParcelPose sanctuaryParcelPose(float filmTime, float motionScale) noexcept;
inline constexpr uint32_t kShenzhenRelay = 86;
inline constexpr uint32_t kShenzhenRelayReceiver = 87;
inline constexpr uint32_t kShenzhenRelayHose = 88;
struct RelayFlyby {
    std::array<float,3> center;
    float yaw, pitch, bank, cameraWeight;
};
// Both the full film and the trilogy express the Shenzhen sequence in 60 local seconds.
RelayFlyby relayFlyby(float localSeconds, float motionScale) noexcept;
enum class CityBirdSpecies { BlackKite, Egret, NightHeron };
// Each body is followed by the left/right inner wings and the left/right hands.
// Bird-local axes are +Z forward and +Y up. Both hinges turn about local Z.
inline constexpr uint32_t kKiteBody = 160, kEgretBody = 165, kNightHeronBody = 192;
inline constexpr float kBirdShoulderX = .09f, kBirdWristX = .52f;
Bmesh buildCityBird(CityBirdSpecies species);
inline constexpr uint32_t kSkyBlimp = 172u;
struct BlimpSignFace {
    std::wstring_view text;
    uint32_t width, height;
    std::string_view rows; // Hex nibbles, four pixels per digit, padded at each row end.
};
struct BlimpSigns {
    std::string_view languageTag;
    BlimpSignFace front, back;
};
const BlimpSigns& cityBlimpSigns(std::string_view languageTag) noexcept;
Bmesh buildCityBlimp(const BlimpSigns& signs = cityBlimpSigns("en-US"));
std::array<float,3> sanctuaryCourierPosition(float filmTime,float motionScale=1.f,float* heading=nullptr) noexcept;
uint32_t selectCityModuleLod(float distanceWorld, float worldUnitsPerMeter, uint32_t seed) noexcept;
float cityDetailScale(float distanceWorld, float worldUnitsPerMeter, uint32_t seed, uint32_t detailTier) noexcept;
} // namespace boo
