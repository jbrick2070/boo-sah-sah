#pragma once

namespace boo {
// Native UI, preview and both exports share the same localized vocabulary.
enum class ResultText {
    Run,                 // Run benchmark
    Select,              // Select a test
    Raster,              // Raster
    Path,                // Path tracing (High)
    Dlss,                // DLSS (Quality)
    Preparing,           // Preparing
    Warming,             // Warming up
    Checking,            // Checking
    Finishing,           // Finishing
    Cancel,              // Cancel
    Back,                // Back to film
    Close,               // Close
    SavePicture, SaveText,
    ReportButton,              // Open report page
    ConfirmReport,       // GitHub issues are public. You choose what to attach and submit. Open the report page?
    FileSaveFailed,          // Could not save both files. Choose another local folder and try again.
    PictureSaved, TextSaved,
    Results,             // Benchmark results
    Completed,           // Completed
    Interrupted,         // Interrupted
    Cancelled,           // Cancelled
    Unavailable,         // Unavailable
    Unknown,             // Unknown
    Busy,                // Another graphics task is running.
    Software,            // This test requires a hardware GPU.
    Unsupported,         // This GPU does not support this test.
    NoDxr,               // This test requires DXR 1.1.
    NoDlss,              // DLSS is unavailable in this build.
    Unconfirmed,         // Support could not be confirmed.
    Memory,              // Insufficient graphics memory budget.
    Timeout,             // Time limit reached. Reopen the film to continue.
    Reset,               // Graphics device reset. Reopen the film to continue.
    RenderFailed,        // The renderer could not complete the test.
    TimingFailed,        // GPU timings are incomplete.
    Changed,             // The workload changed during the test.
    WorkerExited,        // The benchmark worker stopped.
    Build,               // Build
    Outcome,             // Outcome
    Driver,              // Display driver
    Architecture,        // Architecture (Windows / app)
    Power,               // Power
    Mains,               // Mains
    Battery,             // Battery
    PowerChanged,        // Changed
    Workload,            // Planned workload
    Canvas,              // Render resolution
    Warmup,              // Warm-up frames
    Samples,             // Valid samples
    Typical,             // Typical GPU frame (ms)
    Worst,               // Worst GPU frame (ms)
    ColdStart, WarmStart, NotMeasuredYet, ThisLaunch,
    Renderer,            // Film renderer
    Lighting,            // Lighting
    Particles,           // Particles
    Neural,              // Neural material
    Director,            // Director
    Codec,               // Codec
    Ran,                 // Ran
    Fallback,            // Fallback
    Off,                 // Off by preset
    NotExercised,        // Not exercised
    Failed,              // Failed
    Scripted,            // Scripted
    ConditionsLatest,          // Frame times cover this shot. Startup is for this launch.
    SafeFamily,          // Safe preset · Family edition
    Community,           // Community reported
    Metric, // Measurement
    ThisRun, // This run
    Reference, // RTX 5080 reference
    NoReference, // No matching reference run
    LowerIsBetter, // A lower GPU frame time is faster.
    NoNpu, // No NPU detected
    Details, HideDetails, DlssNote, DlssRanGpu, DlssDidNotRunGpu,
    SameAsReference, Faster, Slower,
    Count
};

const wchar_t* resultText(ResultText key);
} // namespace boo
