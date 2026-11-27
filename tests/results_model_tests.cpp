#include "boo/results_model.hpp"

#include <cmath>
#include <iostream>
#include <limits>

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
    return failures == 0 ? 0 : 1;
}
