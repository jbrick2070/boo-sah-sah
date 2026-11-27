#include "boo/results_dialog.hpp"
#include "boo/results_worker.hpp"
#include "boo/ui_text.hpp"
#include <windows.h>
#include <string_view>

namespace boo {
// The fixture submits a failure snapshot across the production IPC boundary; it never opens a D3D device.
void runResultsBenchmark(const BenchmarkRequest& request, BenchmarkReporter& reporter) {
    BenchmarkSnapshot snap{};
    snap.mode = request.mode;
    snap.stage = BenchmarkStage::Measuring;
    snap.warmupCount = kResultsWarmupCount;
    snap.measuredCount = 9;
    if (!reporter.publish(snap)) return;
    if (request.luidLow == 2) {
        snap.outcome=ResultsOutcome::Completed;
        snap.stage=BenchmarkStage::Finished;
        snap.width=snap.inputWidth=1280;
        snap.height=snap.inputHeight=720;
        snap.power=BenchmarkPower::Mains;
        snap.measuredCount=kResultsMeasuredCount;
        snap.medianMs=9.4;
        snap.worstMs=11.6;
        snap.evidence={BenchmarkEvidence::FilmGpu,BenchmarkEvidence::RasterGpu,BenchmarkEvidence::HlslGpu,
            BenchmarkEvidence::Unknown,BenchmarkEvidence::ScriptedCpu,BenchmarkEvidence::Off,BenchmarkEvidence::Off};
        reporter.finish(snap);
        return;
    }
    snap.reason = BenchmarkReason::GraphicsReset;
    snap.outcome = ResultsOutcome::Interrupted;
    snap.stage = BenchmarkStage::Finishing;
    if (!reporter.publish(snap)) return;
    if (request.luidLow == 1) Sleep(INFINITE); // A removed device's teardown may never return.
    snap.stage = BenchmarkStage::Finished;
    reporter.publish(snap);
}
} // namespace boo

int main(int argc, char** argv) {
    if (int child = boo::runResultsWorkerIfRequested(argc, argv); child >= 0) return child;
    if (argc < 3 || std::string_view(argv[1]) != "--no-activate" || std::string_view(argv[2]) != "--mute") return 2;
    if (!boo::UiStrings::instance().load(L"assets/strings","en-US")) return 4;
    boo::ResultsDialogContext context;
    context.hardware={boo::ResultsCpuModel::IntelCoreUltra9_275HX,boo::ResultsNpuModel::IntelAiBoost};
    const bool comparison=argc==4 && std::string_view(argv[3])=="--comparison";
    if (comparison) context.hardware={boo::ResultsCpuModel::AMD,boo::ResultsNpuModel::AmdRyzenAI};
    context.adapter.luidLow = comparison ? 2 : (argc==4 && std::string_view(argv[3])=="--stall-cleanup" ? 1 : 0);
    context.availability[0] = boo::BenchmarkReason::None;
    return boo::openResultsDialog(nullptr, context)==!comparison ? 0 : 3;
}
