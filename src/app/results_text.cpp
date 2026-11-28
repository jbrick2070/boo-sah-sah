#include "boo/results_text.hpp"
#include "boo/ui_text.hpp"

#include <array>
#include <cstddef>

namespace boo {
namespace {
struct Entry {
    const char* key;
    const wchar_t* english;
};
constexpr auto entries = std::to_array<Entry>({
    {"results.Run", L"Run benchmark"},
    {"results.Select", L"Select a test"},
    {"results.Raster", L"Raster"},
    {"results.Path", L"Path tracing (High)"},
    {"results.Dlss", L"DLSS (Quality)"},
    {"results.Preparing", L"Preparing"},
    {"results.Warming", L"Warming up"},
    {"results.Checking", L"Checking"},
    {"results.Finishing", L"Finishing"},
    {"results.Cancel", L"Cancel"},
    {"results.Back", L"Back to film"},
    {"results.Close", L"Close"},
    {"results.SavePicture", L"Save picture…"},
    {"results.SaveText", L"Save text…"},
    {"results.ReportButton", L"Report…"},
    {"results.ConfirmReport", L"GitHub issues are public. You choose what to attach and submit. Open the report page?"},
    {"results.FileSaveFailed", L"Could not save the file. Choose another location and try again."},
    {"results.PictureSaved", L"Picture saved."},
    {"results.TextSaved", L"Text saved."},
    {"results.Results", L"Benchmark results"},
    {"results.Completed", L"Completed"},
    {"results.Interrupted", L"Interrupted"},
    {"results.Cancelled", L"Cancelled"},
    {"results.Unavailable", L"Unavailable"},
    {"results.Unknown", L"Unknown"},
    {"results.Busy", L"Another graphics task is running."},
    {"results.Software", L"This test requires a hardware GPU."},
    {"results.Unsupported", L"This GPU does not support this test."},
    {"results.NoDxr", L"This test requires DXR 1.1."},
    {"results.NoDlss", L"DLSS is unavailable in this build."},
    {"results.Unconfirmed", L"Support could not be confirmed."},
    {"results.Memory", L"Insufficient graphics memory budget."},
    {"results.Timeout", L"Time limit reached. Reopen the film to continue."},
    {"results.Reset", L"Graphics device reset. Reopen the film to continue."},
    {"results.RenderFailed", L"The renderer could not complete the test."},
    {"results.TimingFailed", L"GPU timings are incomplete."},
    {"results.Changed", L"The workload changed during the test."},
    {"results.WorkerExited", L"The benchmark worker stopped."},
    {"results.Build", L"Build"},
    {"results.Outcome", L"Outcome"},
    {"results.Driver", L"Display driver"},
    {"results.Architecture", L"Architecture (Windows / app)"},
    {"results.Power", L"Power"},
    {"results.Mains", L"Mains"},
    {"results.Battery", L"Battery"},
    {"results.PowerChanged", L"Changed"},
    {"results.Workload", L"Planned workload"},
    {"results.Canvas", L"Render resolution"},
    {"results.Warmup", L"Warm-up frames"},
    {"results.Samples", L"Valid samples"},
    {"results.Typical", L"Typical GPU frame (ms)"},
    {"results.Worst", L"Worst GPU frame (ms)"},
    {"results.ColdStart", L"Cold start (s)"},
    {"results.WarmStart", L"Warm start (s)"},
    {"results.NotMeasuredYet", L"Not measured yet"},
    {"results.ThisLaunch", L"this launch"},
    {"results.Renderer", L"Film renderer"},
    {"results.Lighting", L"Lighting"},
    {"results.Particles", L"Particles"},
    {"results.Neural", L"Neural material"},
    {"results.Director", L"Director"},
    {"results.Codec", L"Codec"},
    {"results.Ran", L"Ran"},
    {"results.Fallback", L"Fallback"},
    {"results.Off", L"Off by preset"},
    {"results.NotExercised", L"Not exercised"},
    {"results.Failed", L"Failed"},
    {"results.Scripted", L"Scripted"},
    {"results.ConditionsLatest", L"Frame times cover this shot. Startup times are the latest measurements."},
    {"results.SafeFamily", L"Safe preset · Family edition"},
    {"results.Community", L"Community reported"},
    {"results.Metric", L"Measurement"},
    {"results.ThisRun", L"This run"},
    {"results.Reference", L"RTX 5080 reference"},
    {"results.NoReference", L"No matching reference run"},
    {"results.LowerIsBetter", L"A lower GPU frame time is faster."},
    {"results.NoNpu", L"No NPU detected"},
    {"results.Details", L"Details"},
    {"results.HideDetails", L"Hide details"},
    {"results.DlssNote", L"DLSS adds a fixed cost per frame. At this 1280 x 720 test size that can cost more than it saves; it pays off at higher resolutions and with path tracing."},
    {"results.DlssRanGpu", L"DLSS ran on the GPU"},
    {"results.DlssDidNotRunGpu", L"DLSS did not run on the GPU"},
    {"results.SameAsReference", L"About the same as the RTX 5080 reference"},
    {"results.Faster", L"{n}% faster than the RTX 5080 reference"},
    {"results.Slower", L"{n}% slower than the RTX 5080 reference"},
});
static_assert(entries.size() == static_cast<size_t>(ResultText::Count));
} // namespace

const wchar_t* resultText(ResultText id) {
    const auto& entry = entries.at(static_cast<size_t>(id));
    const auto* translated = UiStrings::instance().find(entry.key);
    return translated ? translated->c_str() : entry.english;
}
} // namespace boo
