#include "boo/results_model.hpp"
#include "boo/results_benchmark.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <utility>
#include <vector>

namespace {
int failures = 0;
void check(bool value, int line) {
    if (!value) { std::cerr << "results model check failed at line " << line << '\n'; ++failures; }
}
#define CHECK(condition) check((condition), __LINE__)

void fill(boo::ResultsMeasurements& run) {
    for (uint32_t i = 0; i < boo::kResultsMeasuredCount; ++i)
        CHECK(run.record(i + static_cast<uint32_t>(boo::kResultsWarmupCount), static_cast<double>(i)));
}
}

int main() {
    using namespace boo;
    BenchmarkSnapshot progress{};
    CHECK(resultsProgress(progress).marquee && resultsProgress(progress).position == 0);
    progress.stage = BenchmarkStage::Warming;
    // One bar across 30 warm-up and 180 measured frames: it never goes backwards.
    progress.warmupCount = 15;
    CHECK(!resultsProgress(progress).marquee && resultsProgress(progress).position == 71);
    progress.warmupCount = 31;
    const uint32_t endOfWarmup = resultsProgress(progress).position;
    CHECK(endOfWarmup == 142);
    progress.stage = BenchmarkStage::Measuring;
    progress.measuredCount = 0;
    CHECK(resultsProgress(progress).position >= endOfWarmup);
    progress.measuredCount = 45;
    CHECK(resultsProgress(progress).position == 357);
    progress.measuredCount = 180;
    CHECK(resultsProgress(progress).position == 1000);
    progress.mode = BenchmarkMode::RenderTest;
    progress.tilesDone = 2;
    progress.tilesTotal = 4;
    CHECK(resultsProgress(progress).position == 500);
    progress.tilesDone = 10;
    CHECK(resultsProgress(progress).position == 1000);
    progress.tilesTotal = 0;
    CHECK(resultsProgress(progress).position == 0);
    progress.tilesTotal = kResultsMaxRenderTiles + 1;
    CHECK(resultsProgress(progress).position == 0);
    progress.stage = BenchmarkStage::Finishing;
    CHECK(!resultsProgress(progress).marquee && resultsProgress(progress).position == 1000);
    progress.stage = BenchmarkStage::Finished;
    CHECK(resultsProgress(progress).position == 1000);

    // Preserve the authored scene order, including the earlier Shanghai insert.
    constexpr std::array<double, 6> expectedTourTimes{3.5, 47.0, 225.0, 86.0, 298.0, 343.5};
    CHECK(kFilmTourTimesSeconds == expectedTourTimes);
    // Every scene freezes its authored film time for warm-up and measurement.
    for (size_t scene = 0; scene < kFilmTourSceneCount; ++scene) {
        const auto first = filmTourSampleAt(scene, 0);
        CHECK(first && first->sample48k >= 0 && first->sample48k < 360 * 48000);
        for (uint32_t frame = 0; frame < kFilmTourFramesPerScene; ++frame) {
            const auto sample = filmTourSampleAt(scene, frame);
            const auto repeat = filmTourSampleAt(scene, frame);
            CHECK(sample && repeat && first && sample->sample48k == first->sample48k &&
                  sample->sample48k == repeat->sample48k && sample->tag == repeat->tag &&
                  sample->kind == (frame < kFilmTourWarmupCount ? ResultsSampleKind::Warmup : ResultsSampleKind::Measured));
        }
        CHECK(!filmTourSampleAt(scene, kFilmTourFramesPerScene));
    }
    CHECK(!filmTourSampleAt(kFilmTourSceneCount, 0));
    const auto tourScore = filmTourGeometricMean({1, 2, 4, 8, 16, 32});
    CHECK(tourScore && std::abs(*tourScore - std::sqrt(32.0)) < 1e-12);
    CHECK(!filmTourGeometricMean({1, 2, 4, 8, 16, 0}));
    CHECK(!filmTourGeometricMean({1, 2, 4, 8, 16, -1}));
    CHECK(!filmTourGeometricMean({1, 2, 4, 8, 16, std::numeric_limits<double>::infinity()}));
    CHECK(!filmTourGeometricMean({1, 2, 4, 8, 16, std::numeric_limits<double>::quiet_NaN()}));

    BenchmarkSnapshot tourProgress{};
    tourProgress.mode = BenchmarkMode::FilmTour;
    CHECK(resultsProgress(tourProgress).marquee);
    uint32_t previous = 0;
    constexpr auto totalTourFrames = static_cast<uint32_t>(kFilmTourSceneCount) * kFilmTourFramesPerScene;
    for (uint32_t frame = 0; frame <= totalTourFrames + 2; ++frame) {
        tourProgress.tourFramesDone = frame;
        tourProgress.stage = frame % kFilmTourFramesPerScene < kFilmTourWarmupCount ?
            BenchmarkStage::Warming : BenchmarkStage::Measuring;
        const auto current = resultsProgress(tourProgress);
        CHECK(!current.marquee && current.position >= previous && current.position <= 1000);
        previous = current.position;
    }
    CHECK(previous == 1000);
    tourProgress.stage = BenchmarkStage::Finished;
    CHECK(resultsProgress(tourProgress).position == 1000);
    tourProgress.tourFramesDone = kFilmTourFramesPerScene;
    CHECK(resultsProgress(tourProgress).position == 166);
    for (std::size_t i = 0; i < kResultsSampleCount; ++i) {
        const auto sample = resultsSampleAt(i);
        CHECK(sample.has_value());
        CHECK(sample->tag == i);
        CHECK(sample->sample48k == 336000 + static_cast<int64_t>(i) * 1600);
        CHECK(sample->kind == (i < 30 ? ResultsSampleKind::Warmup : ResultsSampleKind::Measured));
    }
    CHECK(!resultsSampleAt(kResultsSampleCount));
    CHECK(resultsSampleAt(29)->sample48k == 382400);
    CHECK(resultsSampleAt(30)->sample48k == 384000);
    CHECK(resultsSampleAt(209)->sample48k == 670400);

    ResultsMeasurements complete;
    fill(complete);
    CHECK(complete.validSampleCount() == 180);
    auto result = complete.finish(ResultsOutcome::Completed);
    CHECK(result.outcome == ResultsOutcome::Completed && result.summary.has_value());
    CHECK(result.summary->medianMs == 89.5 && result.summary->worstMs == 179.0);
    CHECK(!complete.record(30, 5.0));
    CHECK(complete.finish(ResultsOutcome::Cancelled).outcome == ResultsOutcome::Completed);

    ResultsMeasurements incomplete;
    CHECK(incomplete.record(30, 0.0));
    CHECK(incomplete.validSampleCount() == 1);
    result = incomplete.finish(ResultsOutcome::Completed);
    CHECK(result.outcome == ResultsOutcome::Unavailable && !result.summary);

    for (auto reason : {ResultsInvalidation::MissingQuery, ResultsInvalidation::CanvasChanged, ResultsInvalidation::EmergencyImage}) {
        ResultsMeasurements run;
        fill(run);
        run.invalidate(reason);
        result = run.finish(ResultsOutcome::Completed);
        const auto expected = reason == ResultsInvalidation::MissingQuery ? ResultsOutcome::Unavailable : ResultsOutcome::Interrupted;
        CHECK(result.outcome == expected && !result.summary);
    }
    for (auto outcome : {ResultsOutcome::Interrupted, ResultsOutcome::Cancelled, ResultsOutcome::Unavailable}) {
        ResultsMeasurements run;
        fill(run);
        result = run.finish(outcome);
        CHECK(result.outcome == outcome && !result.summary);
    }
    for (double invalid : {-1.0, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        ResultsMeasurements run;
        CHECK(!run.record(30, invalid));
        fill(run);
        CHECK(!run.finish(ResultsOutcome::Completed).summary);
    }
    ResultsMeasurements duplicate;
    fill(duplicate);
    CHECK(!duplicate.record(30, 2.0));
    CHECK(!duplicate.finish(ResultsOutcome::Completed).summary);
    ResultsMeasurements wrongTag;
    CHECK(!wrongTag.record(29, 0.0));
    CHECK(!wrongTag.record(210, 0.0));
    fill(wrongTag);
    CHECK(!wrongTag.finish(ResultsOutcome::Completed).summary);
    // A clipped edge tile still owns each pixel exactly once. Repeat the order to
    // guard reproducibility and test an odd grid with a unique centre tile.
    for (const auto dimensions : {std::pair{257u, 129u}, std::pair{1920u, 1080u},
                                  std::pair{1u, 1u}}) {
        const auto [width, height] = dimensions;
        const auto tiles = renderTileOrder(width, height, 64);
        const auto again = renderTileOrder(width, height, 64);
        CHECK(!tiles.empty() && tiles.size() == again.size());
        std::vector<unsigned char> covered(static_cast<size_t>(width) * height, 0);
        for (size_t i = 0; i < tiles.size(); ++i) {
            const auto& tile = tiles[i];
            CHECK(tile.x == again[i].x && tile.y == again[i].y &&
                  tile.width == again[i].width && tile.height == again[i].height);
            CHECK(tile.width > 0 && tile.height > 0 && tile.x + tile.width <= width &&
                  tile.y + tile.height <= height);
            for (uint32_t y = tile.y; y < tile.y + tile.height; ++y)
                for (uint32_t x = tile.x; x < tile.x + tile.width; ++x)
                    ++covered[static_cast<size_t>(y) * width + x];
        }
        for (const auto count : covered) CHECK(count == 1);
        const auto distance = [width, height](const RenderTile& tile) {
            const int64_t dx = 2 * static_cast<int64_t>(tile.x) + tile.width - width;
            const int64_t dy = 2 * static_cast<int64_t>(tile.y) + tile.height - height;
            return dx * dx + dy * dy;
        };
        for (const auto& tile : tiles) CHECK(distance(tiles.front()) <= distance(tile));
    }
    CHECK(renderTileOrder(0, 100, 64).empty());
    CHECK(renderTileOrder(100, 0, 64).empty());
    CHECK(renderTileOrder(100, 100, 0).empty());
    const auto rate = renderMegaSamplesPerSecond(1920, 1080, 64, 20.0);
    CHECK(rate && std::abs(*rate - 6.63552) < 1e-9);
    CHECK(!renderMegaSamplesPerSecond(0, 1080, 64, 20.0));
    CHECK(!renderMegaSamplesPerSecond(1920, 1080, 0, 20.0));
    CHECK(!renderMegaSamplesPerSecond(1920, 1080, 64, 0.0));
    CHECK(!renderMegaSamplesPerSecond(1920, 1080, 64, std::numeric_limits<double>::infinity()));
    CHECK(!renderMegaSamplesPerSecond(1920, 1080, 64, std::numeric_limits<double>::quiet_NaN()));
    return failures == 0 ? 0 : 1;
}
