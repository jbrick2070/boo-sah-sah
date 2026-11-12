# Engineering upgrade findings and verification

Date: 2026-11-11. Host: RTX 5080 Laptop GPU, Windows 11 x64.
Scope: prioritized M1 lifecycle, clean-feed quality, and screening controls.
The user explicitly requested native 1440p Showcase, superseding the older
specification's 1080p Showcase preset description for this pass.

## A. Ranked bugs and lifecycle findings

| Priority | Grounded finding | Resolution |
|---|---|---|
| P1 | Original `src/app/main.cpp:375-397` worker dereferenced render-context state and mutable `settings.disableRT`; `X` writes that flag and Present recovery destroys the context. | Capture immutable settings, adapter identity and owning device COM reference before starting. Cancel/join before recovery; exception boundary around worker. Current implementation starts at main.cpp:378. |
| P1 | Original `src/render/d3d12_context.cpp:378-390` and `src/bench/benchmarks.cpp:149-161` ignored fence wait results. A timeout could lead to allocator reuse or uncompleted readback. | Wait for actual completion while checking device removal; fail closed on signal/event failures. Do not treat a five-second timeout as completion. No forced interruption of an active GPU submission. |
| P1 | Original `src/app/main.cpp:585-587` ignored ResizeBuffers failure after releasing old targets. | Stop with nonzero exit before using invalid backbuffers. Presentation recovery is limited to one attempt; begin-frame device failure also terminates instead of spinning. |
| P1 | Live M1 used stateful `ScriptedDirector::evaluate` smoothing (`src/inference/scripted_director.cpp`, initialization/lerp branches) while headless tests supplied fixed mood values. | M1 application path now evaluates a fresh sample-derived director using analytical audio. Legacy M0 behavior retained. |
| P2 | Shared TLAS/scratch storage was rebuilt without an explicit pre-build UAV dependency on previous traversal/scratch use. | Added pre-build TLAS and scratch UAV barriers; retained post-build visibility barrier. Especially relevant to the extra A/B clean restoration pass. |
| P2 | `DXGI_STATUS_OCCLUDED` was counted as an ordinary present. | Track occlusion, probe with PRESENT_TEST, and throttle hidden rendering; count only visible successful presents. |
| P2 | Fullscreen SetWindowPos calls omitted SWP_NOACTIVATE; Alt+Enter repeat could toggle repeatedly. | Preserve nonactivation in both transitions and suppress repeated Alt+Enter. |
| P2 | WASAPI failure already set health false and clock fallback already preserved position, but live telemetry kept using stale audio-band snapshots. | Main loop notices unhealthy audio even while paused, switches clocks, uses analytical bands and records an invalidation reason. |
| P2 | Benchmark sweep completion state was reused across sweeps. | Clear completion at launch, reset badge application, avoid publishing a cancelled sweep as completed. |
| P2 | Hardware benchmark admission relied mainly on feature/runtime support. | Explicit adapter software/unknown guard disables hardware stages; applying badges requires executed + verified + Accelerated. |

Existing minimize/zero-size protection, resize-message coalescing, WM_DPICHANGED
handling and supported tearing flags were present and retained. They were not
reported as newly invented fixes.

## B. Visual and silicon upgrades

- CanvasClean RTV, SRV and composite PSO now use R10G10B10A2_UNORM, with 10-bit
  sample-seeded dither. FP16 scene and bloom remain intact.
- Native Safe 720p, Balanced 1080p, Showcase 1440p and Experimental 4K. Transactional
  VRAM admission and Safe fallback remain. Window work-area clamping is separate.
- Packed RGB10 readback is available to tests. Existing RGBA8 test callers use
  explicit integer-rounded channel conversion. Exact RGB10 and RGBA8 seek equality
  is tested, including evidence of intermediate 10-bit values.
- Wavelength-dependent Cauchy indices drive a modest thin-film color approximation
  on petals. This is not a full spectral transmission integrator.
- Added low-energy tinted horizontal bloom streaks and guarded normal/half-vector
  normalization against zero-length input.
- Live A/B renders DXR left and analytic raster right, with divider and labels.
  NO RT is shown when hardware execution is unavailable. This compares different
  geometric representations, not a scientifically isolated reflection-only toggle.
- A/B then rerenders canonical CanvasClean after the presentation blit. This
  deliberately pays additional GPU cost to keep diagnostics out of the clean feed.
  That restoration cost is included in the final presentation timing segment.

## C. Screening and inspection controls

| Input | Behavior |
|---|---|
| Comma / period, [ / ] | Paused-only -/+800 samples at 48 kHz, clamped to timeline bounds |
| Left / right | -/+2 seconds |
| Shift + left / right | -/+10 seconds |
| G | 43-second pre-roll |
| Tab / F1 | Full / compact / A/B / off |
| Minus / equals, down / up | 5% volume trim, bounded 0-100%, two-second HUD indication |
| F12 | Canonical HUD-free BMP and JSON sidecar |

Compact HUD: timecode, five silicon status indicators, GPU milliseconds and
16-sample GPU timing sparkline. Existing R/N/P, modes, CLI and M0 cue routing remain.
BMP is explicitly 8-bit; sidecar identifies cleanBits=10 and exportBits=8. Sidecar
GPU timings describe the last completed presentation, not a fictitious timestamp
measurement of the synchronous capture. Capture re-renders the exact sample and
can briefly stall the render thread.

## Verification

Both x64-base-release and x64-base-debug built with no compiler/shader warnings or
errors in the recorded build logs. Both final CTest runs passed all three suites:
10/10 unit checks, six valid cue sheets, and 7/7 integration checks. Representative
final pre-smoke times: release 3.83 seconds; debug 4.88 seconds.

New checks cover paused steps and seek bounds, native 1440p/4K allocation, exact
packed RGB10/RGBA8 replay, A/B deterministic output, full/compact/off clean parity,
BMP pixels and JSON metadata, cancellation before a suite starts, concurrent
secondary-queue benchmarking with rendering/resizing/HUD changes, key dispatch
and repeat suppression, minimize/restore, fullscreen nonactivation, and zero
D3D12 error/corruption messages in the new render tests. Audio integration now
shuts down a real initialized engine during playback and verifies monotonic
fallback and paused stability. This is not a physical headphone-unplug test.

Release and debug `scripts/run_smoke.ps1` runs pass. Every GUI launch uses
`--no-activate`. The final smoke repeats skip standalone benchmarks because the
full release smoke already ran and verified them. `dumpbin /dependents` passes for
both executables: no CUDA, cuBLASLt, OpenVINO, NVENC API or Spout hard imports.
The smoke script now also rejects a failed dumpbin invocation.

### Measured GPU timings

Isolated release runs; 10-bit SDR swapchain, vsync enabled. Per-pass columns are
last retired frame timings, while p50/p95 summarize the run; they need not sum to
the median. Native canvas resolution is asserted separately by integration tests.
Windowed swapchain dimensions may be clamped by the monitor work area.

| Canvas / case | Frames | GPU p50 / p95 ms | Scene ms | Bloom ms | Composite ms | Present ms | Effect faults |
|---|---:|---:|---:|---:|---:|---:|---:|
| Balanced 1080p | 603 | 0.432 / 0.437 | 0.348 | 0.023 | 0.019 | 0.038 | 0 |
| Showcase native 1440p | 363 | 0.702 / 0.712 | 0.567 | 0.035 | 0.033 | 0.071 | 0 |
| Experimental native 4K | 363 | 1.768 / 1.842 | 1.501 | 0.115 | 0.086 | 0.147 | 0 |
| WARP Safe 720p | 58 | 29.419 / 38.256 | 15.809 | 3.829 | 3.564 | 6.096 | 0 |

The 4K and 1440p runs verified live DXR output. WARP reported Substituted and did
not claim hardware acceleration. A 500-iteration benchmark-on-start run with a
0.1-second exit request shut down in 0.113 seconds, presenting 14 frames. This
single observation is not a universal shutdown latency guarantee.

Standalone release benchmarks (30 timed passes): verified DXR 17,600 MRays/s,
CUDA 37,004 MPart/s, Tensor 91.33 TFLOPS, hardware H.264 359.61 FPS. These are
workload-specific scores, not application throughput or comparable codec/shader work.

### Evidence paths

- `build/engineering_build_release.log`, `build/engineering_build_debug.log`
- `build/engineering_ctest_release.log`, `build/engineering_ctest_debug.log`
- `build/engineering_smoke_release.log` (full benchmark run)
- `build/engineering_smoke_release_final.log`, `build/engineering_smoke_debug_final.log`
- `build/x64-release/diagnostics/engineering_1440p.json`, `engineering_4k.json`,
  `engineering_warp.json`, `engineering_cancel.json`
- `build/x64-release/diagnostics/benchmarks_latest.json` and CSV
- `diagnostics/test_captures/` contains test BMP/JSON artifacts; a 3840x2160 reveal
  export was visually inspected and its pixels independently compared in tests.

## Limits and next priorities

1. HDR10/scRGB output remains optional follow-up: this pass is true RGB10 SDR,
   not HDR. No unverified monitor HDR claim is made.
2. Physical endpoint hot-unplug/default-device migration, mixed-DPI monitor dragging,
   real occlusion transitions and forced device removal/TDR were not exercised on
   this desktop. Existing endpoint failure handling was tested through reader
   failure and engine shutdown. No automatic endpoint migration is implemented.
3. WASAPI's clock currently counts submitted frames, not IAudioClock presentation
   position. Endpoint buffering can therefore create bounded audio/display offset.
   Seek uses the existing fade-in; acoustic clicklessness was not instrumentally
   verified. A true output-clock and crossfade audit is worthwhile follow-up.
4. GPU waits preserve resource lifetime until completion/device removal. A driver
   call that never returns or a GPU hang without device removal can delay shutdown;
   cooperative cancellation cannot guarantee a hard deadline.
5. RayQuery area-light shadows/AO, true volumetric integration, spectral refraction
   transport, and extra compute caustics were not added. Existing wet-ripple and
   sky/mist approximations remain. These are next visual experiments, not claimed
   delivered features.
6. The repository had no commits and all project files were untracked at entry.
   No commit was created. An encoding error truncated the untracked README during
   a documentation edit; it was reconstructed from verified project sources rather
   than restored byte-for-byte. Its prior wording is not recoverable from Git.
