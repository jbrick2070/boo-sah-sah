#pragma once

// The PhysX suite's shared foundation: fixed-step timing everywhere, and in builds with the PhysX SDK
// the runtime itself, started in the background on this machine's GPU.

#include "boo/baked_physics.hpp"
#include "boo/canopy_cloth.hpp"
#include "boo/dawn_bathroom.hpp"
#include "boo/physx_marbles.hpp"
#include "boo/logging.hpp"
#include "boo/wind_field.hpp"
#include "boo/physx_genres.hpp"
#include "boo/physx_stepper.hpp"
#include "boo/physx_suite.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include <dxgi1_6.h>
#include <wrl/client.h>

namespace {

// The version word of a .bphys file.
inline uint32_t bakedFileVersion(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    char head[8]{};
    file.read(head, 8);
    uint32_t version = 0;
    std::memcpy(&version, head + 4, 4);
    return version;
}

bool testPhysxSuite() {
    using namespace boo;

    // The genre table drives the dial: it wraps round, and off is first.
    BOO_CHECK(kPhysxGenreCount >= 2u && physxGenreInfo(0).key == "off");
    BOO_CHECK(nextPhysxGenre(kPhysxGenreCount - 1u) == 0u);
    BOO_CHECK(physxGenreInfo(999u).key == "off");

    // Fixed steps: 60 fps frames at a 1/120 s step take two steps each, steadily.
    {
        PhysxStepper stepper(1.0 / 120.0, 4u);
        uint32_t steps = 0;
        for (int i = 0; i < 60; ++i) steps += stepper.advance(1.0 / 60.0, false, false);
        BOO_CHECK(steps >= 119u && steps <= 120u);
        BOO_CHECK(stepper.owedSeconds() < stepper.stepSeconds());
    }
    // A slow frame takes at most four steps and gives up the rest instead of snowballing.
    {
        PhysxStepper stepper(1.0 / 120.0, 4u);
        BOO_CHECK(stepper.advance(0.2, false, false) == 4u);
        BOO_CHECK(stepper.droppedSeconds() > 0.15 && stepper.owedSeconds() == 0.0);
        BOO_CHECK(stepper.advance(1.0 / 120.0 + 1e-9, false, false) == 1u);
    }
    // Paused frames take no steps and owe nothing; a reset forgets what was owed.
    {
        PhysxStepper stepper(1.0 / 120.0, 4u);
        BOO_CHECK(stepper.advance(1.0 / 60.0, true, false) == 0u && stepper.owedSeconds() == 0.0);
        stepper.advance(0.005, false, false); // less than a step: owed
        BOO_CHECK(stepper.owedSeconds() > 0.0);
        BOO_CHECK(stepper.advance(0.0, false, true) == 0u && stepper.owedSeconds() == 0.0 && stepper.resets() == 1u);
    }

    // Baked tracks: a round trip through the file format, interpolation between keys, and the film's
    // own winch track present, covering the lowering, and ending on the authored dock pose.
    {
        BakedPhysics out;
        BakedTrack t;
        t.name = "test";
        t.firstTime = 10.f;
        t.keyRate = 2.f;
        t.keys.push_back({{0.f, 0.f, 0.f}, {0.f, 0.f, 0.f, 1.f}});
        t.keys.push_back({{2.f, 4.f, 6.f}, {0.f, 0.f, 0.f, 1.f}});
        out.tracks.push_back(t);
        const auto path = std::filesystem::temp_directory_path() / "boo_baked_test.bphys";
        BOO_CHECK(saveBakedPhysics(path, out));
        BOO_CHECK(bakedFileVersion(path) == 1u); // rigid tracks only: the committed bakes keep their bytes
        BakedPhysics in;
        BOO_CHECK(loadBakedPhysics(path, in) && in.tracks.size() == 1u && in.tracks[0].name == "test");
        const BakedPose mid = in.tracks[0].sample(10.25f);
        BOO_CHECK(std::abs(mid.position[1] - 2.f) < 1e-5f && in.tracks[0].covers(10.5f) && !in.tracks[0].covers(11.f));
        std::error_code ec;
        std::filesystem::remove(path, ec);
        const BakedTrack* winch = filmBakedPhysics().find("parcel_case");
        BOO_CHECK(winch && winch->covers(170.f) && winch->lastTime() >= 173.4f);
        // A file with only rigid tracks stays version 1, so the committed bakes keep their bytes.
    }
    // Grid tracks (deforming surfaces): version 2, exact at keys, Catmull-Rom between, clamped outside;
    // and the film's cloth canopy present over both spans the ribbons are on screen.
    {
        BakedPhysics out;
        BakedGridTrack g;
        g.name = "grid";
        g.firstTime = 5.f;
        g.keyRate = 10.f;
        g.offsetScale = 0.001f;
        g.nodes = 2;
        for (int k = 0; k < 4; ++k)
            for (int n = 0; n < 2; ++n)
                for (int axis = 0; axis < 3; ++axis) g.offsets.push_back(int16_t(100 * k + 10 * n + axis));
        out.grids.push_back(g);
        const auto path = std::filesystem::temp_directory_path() / "boo_grid_test.bphys";
        BOO_CHECK(saveBakedPhysics(path, out) && bakedFileVersion(path) == 2u);
        BakedPhysics in;
        BOO_CHECK(loadBakedPhysics(path, in) && in.grids.size() == 1u && in.findGrid("grid") && in.findGrid("grid")->keyCount() == 4u);
        float at[6]{};
        in.grids[0].sample(5.1f, at); // key 1 exactly
        BOO_CHECK(std::abs(at[0] - 0.100f) < 1e-5f && std::abs(at[5] - 0.112f) < 1e-5f);
        in.grids[0].sample(5.15f, at); // linear data stays linear under Catmull-Rom
        BOO_CHECK(std::abs(at[0] - 0.150f) < 1e-4f);
        in.grids[0].sample(99.f, at); // clamped to the last key
        BOO_CHECK(std::abs(at[0] - 0.300f) < 1e-5f && !in.grids[0].covers(99.f) && in.grids[0].covers(5.3f));
        std::error_code ec;
        std::filesystem::remove(path, ec);
        for (const auto& span : kCanopySpans) {
            const BakedGridTrack* cloth = filmBakedPhysics().findGrid(span.track);
            BOO_CHECK(cloth && cloth->nodes == uint32_t(kCanopyTrackNodes) && cloth->covers(span.first) && cloth->lastTime() >= span.last - 0.11f);
        }
        // The cloth grid's rest shape is the generator's lamella: its feet stand on the vault floor.
        const auto foot = canopyRibbonPoint(0, 0.f, 0.f);
        BOO_CHECK(std::abs(foot[0] + 4.6f) < 1e-5f && std::abs(foot[1] - 3.f) < 1e-5f && std::abs(foot[2] - 4.f) < 1e-5f);
    }
    // The wind: never reversing, slower near the ground, the orchid's own breeze term.
    {
        const float low[3] = {0.f, 1.f, 0.f}, high[3] = {0.f, 60.f, 0.f};
        for (float t = 0.f; t < 60.f; t += 0.5f) {
            const auto a = windAt(low, t), b = windAt(high, t);
            BOO_CHECK(a[0] * kWindHeading[0] + a[2] * kWindHeading[2] > 0.f);
            BOO_CHECK(std::hypot(b[0], b[2]) > std::hypot(a[0], a[2]));
        }
        BOO_CHECK(terraceBreeze(1.f, 0.f) == std::sin(1.f * .78f + 0.f) + .34f * std::sin(1.f * 1.37f + .8f * 0.f));
    }

    // The runtime. Without the SDK it says so; with it, it starts in the background and passes a
    // falling-box check on this machine (on the GPU solvers when CUDA is there).
    {
        PhysxSuite suite(0, 0); // LUID 0: no adapter match, so CPU solvers; enough for the foundation
        const PhysxRuntimeInfo before = suite.info();
#if defined(BOO_UNIT_HAS_PHYSX)
        BOO_CHECK(before.state == PhysxRuntimeState::Stopped);
        suite.requestStart();
        BOO_CHECK(suite.waitUntilSettled(60000));
        const PhysxRuntimeInfo after = suite.info();
        BOO_CHECK(after.state == PhysxRuntimeState::Ready);
        BOO_CHECK(after.sdkVersion.rfind("5.", 0) == 0);
        std::string reason;
        BOO_CHECK(suite.selfTest(&reason));
        // The Explore marbles on PhysX: dropped into the vessel basin they settle inside it, on its
        // porcelain; set down on the counter they stay on the counter; none leaves the room.
        {
            PhysxMarbles marbles(suite, [](float x, float z) { return dawnCountertopHeight(x, z); });
            BOO_CHECK(marbles.ok());
            MarbleState start[8]{};
            for (int i = 0; i < 6; ++i) {
                const float a = float(i) * 1.047f;
                start[i].position[0] = -0.180f + 0.15f * std::cos(a);
                start[i].position[1] = 3.75f + 0.03f * float(i);
                start[i].position[2] = 6.245f + 0.10f * std::sin(a);
                start[i].velocity[0] = -0.4f * std::sin(a);
                start[i].velocity[2] = 0.3f * std::cos(a);
                start[i].radius = 0.021f;
            }
            for (int i = 6; i < 8; ++i) {
                start[i].position[0] = 0.10f + 0.12f * float(i - 6);
                start[i].position[1] = 3.60f;
                start[i].position[2] = 6.12f;
                start[i].radius = 0.02f;
            }
            marbles.reset(true, start, 8);
            MarbleState now[8]{};
            bool moving = true;
            for (int frame = 0; frame < 6 * 60; ++frame) moving = marbles.step(1.0f / 60.0f, now, 8);
            for (int i = 0; i < 8; ++i) {
                const float* p = now[i].position;
                const float surface = dawnCountertopHeight(p[0], p[2]) + now[i].radius;
                const bool inBasin = dawnBasinU(p[0], p[2]) < 0.985f;
                if (i < 6) BOO_CHECK(inBasin && std::abs(p[1] - surface) < 0.012f);
                else BOO_CHECK(!inBasin && std::abs(p[1] - surface) < 0.006f);
                BOO_CHECK(p[1] > 2.6f && p[0] > -2.44f && p[0] < 1.48f && p[2] > 2.3f && p[2] < 6.56f);
            }
            std::cout << "  PhysX marbles: " << marbles.steps() << " steps, " << (moving ? "still rolling" : "at rest") << ", basin marble 0 at y "
                      << now[0].position[1] << "\n";
        }
        suite.stop();
        BOO_CHECK(suite.info().state == PhysxRuntimeState::Stopped && suite.physics() == nullptr);

        // On the film's own GPU: the high-performance adapter, its primary CUDA context, GPU solvers.
        Microsoft::WRL::ComPtr<IDXGIFactory6> factory;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) &&
            SUCCEEDED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))) &&
            SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == 0x10DE) {
            boo::Logger::instance().setConsoleEnabled(true); // show PhysX's own messages if the GPU path fails
            PhysxSuite onGpu(static_cast<int32_t>(desc.AdapterLuid.LowPart), static_cast<int32_t>(desc.AdapterLuid.HighPart));
            onGpu.requestStart();
            BOO_CHECK(onGpu.waitUntilSettled(60000));
            const PhysxRuntimeInfo gpuInfo = onGpu.info();
            BOO_CHECK(gpuInfo.state == PhysxRuntimeState::Ready);
            BOO_CHECK(gpuInfo.gpu && gpuInfo.sharedContext && onGpu.cudaContextManager() != nullptr);
            boo::Logger::instance().setConsoleEnabled(false);
            std::string gpuReason;
            BOO_CHECK(onGpu.selfTest(&gpuReason));
            std::cout << "  PhysX " << gpuInfo.sdkVersion << " GPU solvers on " << gpuInfo.cudaDevice << ", shared CUDA context, started in "
                      << gpuInfo.startMs << " ms\n";
        }
#else
        BOO_CHECK(before.state == PhysxRuntimeState::Unavailable && !before.reason.empty());
        std::string reason;
        BOO_CHECK(!suite.selfTest(&reason));
#endif
    }
    return true;
}

} // namespace
