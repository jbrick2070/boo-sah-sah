# Changelog — BOO SAH SAH: The Glass Orchid Protocol

All notable milestones implemented in accordance with `docs/SPEC_v4.md`.

## [Unreleased] — Film Quality & Robustness

### Added
- Seven-mesh scene pipeline, with new original `enamel_cathedral`, `microbial_sanctuary`, and `dawn_alcove` geometry alongside the four city/orchid assets.
- First-party capsule-SDF presentation lettering: 60 vector glyphs including Latin text, 台北, and Chinese numerals 一–七 paired with Roman act numbers. No external font binary is required.
- Title and act motifs relocate below the right benchmark card in full telemetry mode, keeping the statistics panels clear; two extra layout captures cover this placement.
- Opt-in `boo_integration_tests --film-captures --no-activate --capture-dir <directory>` gate producing 68 native 2560×1440 captures, four montages, and a manifest across both editions, solos, and narrative cards. Checks include seek repeatability, finite HDR output, RT attribution, and clean-feed isolation.
- Opt-in `--film-stress --no-activate --capture-dir <directory>` gate with 224 default iterations, audio/memory/lifecycle measurements, and a synthetic graphics recovery receipt. `--require-debug-layer` fails when the D3D12 debug layer is unavailable; ordinary receipts explicitly distinguish unavailable warning validation from a clean debug run.
- Release and Debug Windows CI for native builds, CPU tests, cue/manifest validation, import audits, and installer lifecycle checks.

### Changed
- Glass shading uses single-ray geometry refraction with environment dispersion approximated separately. Raster presentation remains cohesive but does not reproduce full DXR geometry reflection or multi-bounce spectral transport.
- Score modulation uses integrated oscillator phase, with edition voice crossfades for smoother transitions and late seeks.
- Graphics recovery is limited to one teardown/recreation attempt. `--inject-device-lost` introduces a synthetic fault only after GPU work drains; it is not a physical TDR test.
- Native Win32/LZMS installation runs on a worker thread with cooperative Cancel and rollback. Existing configuration is preserved by default (`--replace-config` opts into packaged defaults), running apps and locked destinations are rejected, and native uninstallation remains available. App and installer use the static MSVC runtime.

Measured verification and hardware-specific limitations are recorded in the [Film Quality & Robustness Report](docs/quality_upgrade_report.md). Feature entries above describe implementation, not a claim that every hardware or debug-layer gate has passed.

## [1.0.0 — Milestone M10] — Portable Release & Complete Hardware Orchestra
### Added
- **M10 Single-App Native Installer & Portable Release**: Added `tools/installer/setup_main.cpp` (`boo_installer_stub.exe`, native Win32 + inbox `cabinet.dll` LZMS self-extracting installer/uninstaller) and `scripts/package_release.ps1` producing a single-file Windows installer `BooSahSah-Setup-win64.exe` (~`12.1 MB`) plus companion `boo-sah-sah-win64-portable.zip` around the unified single application executable `boo_sah_sah.exe` (`--validate-cues`, `--bench`, compiled `.cso` shaders, `.bmesh` geometry, `madame_n` IR model, compact `openvino\` NPU runtime sidecar, cues, configs, `USER_GUIDE.md`, `LICENSES.md`, `CHANGELOG.md`). Updated `scripts/run_smoke.ps1` with automatic `dumpbin.exe` discovery and full M0–M10 verification gates.
- **M9 Optional Signal Hijack Media & Webcam Ingest**: Added strictly opt-in `MediaIngestSource` (`--media-input <path>`, `--camera-input`, `--inject-media-fail`, runtime `[W]` webcam privacy kill-switch) using dynamically loaded `mfreadwrite.dll` (`IMFSourceReader`), audio-stream deselection, `[16..4096]` dimension bounds validation, `1280x720` BT.709 `NV12` resampling, `<USER_HOME>` path redaction, top-center crimson `"LIVE CAMERA"` HUD indicator (visible even when HUD is hidden), live H.264 encode+decode integration, and `SHADER SIGNAL` substitute fallback.
- **M8 Spout2 Clean D3D12 Sender & Live Show Polish**: Added zero-SDK `SpoutSender` (`--enable-spout`, `--disable-spout`, `--spout-name`, runtime `[O]` toggle) publishing `BooSahSah_Clean` (`BGRA8` shared D3D11/D3D12 texture + `SpoutSenderNames` shared memory) strictly from Pass-3 `CanvasClean` with zero HUD or caption contamination and non-blocking receiver-absent resilience.
- **M6 & M7 Six-Minute 7-Act Film, Two Editions, Captions & Combined Failover**: Implemented full `360.000 s` (`17,280,000` samples @ `48 kHz`) choreography across `assets/cues/film_360s.json` (now default in `config/default.toml`), exact narrative reveals at `00:36.000` (enamel-tooth city architecture), `02:55.000` (beneficial microbial ecology), and `05:42.000` (everyday oral-care dawn mirror + non-medical end card), 7-movement procedural score with `Family` / `Midnight` harmonic & caption variants (`[K]`, `--captions` / `--no-captions`), `--reduced-motion`, and mid-show single/multi-accelerator failover without timeline stalls.
- **M5 Madame N / Intel NPU Solo**: Authored deterministic `16 -> 32 -> 16 -> 10` OpenVINO IR stage-director MLP (`tools/gen_madame_n_model.py`, `assets/models/madame_n_director.xml/.bin`), dynamic `openvino_c.dll` loader (`OpenVinoDirector`) targeting explicit `"NPU"` (verified live on physical `Intel(R) AI Boost`) with `"CPU"` and `ScriptedDirector` fallbacks, non-stalling `5 Hz` `AsyncDirectorController`, `--inject-npu-timeout` & `[U]` runtime toggle, and 3D `solo_npu.json` stage.
- **M3 & M4 Standalone CUDA & Tensor Solos**: Upgraded live CUDA PTX simulation to `102,400–262,144` GPU-resident particles (`solo_cuda.json`, `8,192` HLSL linear substitute) with 5 morphing formations, and wired `solo_tensor.json` (`mma.sync.m16n8k16` 64-layer SIREN neural velvet dome + scalar HLSL substitute) and `solo_rt.json` (DXR mirror arch corridor + raster fallback) to dedicated 3D stages.

## [0.3.0 — Milestone M2] — 180-Second Three-City Trilogy & Visual Composition Revision
### Added
- Authored reproducible packed `.bmesh` hero geometry (`shanghai_trestle`, `shenzhen_evtol`, `taipei_101_hero`, `glass_orchid_hero`) and `TrilogyMeshRenderer` shared raster/DXR pipeline across `assets/cues/m2_trilogy_180s.json`.
- Refined Shanghai wet rail viaduct reveal, Shenzhen connected urban ground plane + closer articulated eVTOL tracking + day-to-night lighting transition, botanical Phalaenopsis Glass Orchid petal/sepal/labellum curvature, and unobstructed Taipei 101 + foreground Glass Orchid 176s finale composition.

## [0.2.0 — Milestone M1] — 60-Second Taipei Vertical Slice & Live Four-Pillar Attribution
### Added
- 4-pass HDR->RGB10A2 `CanvasClean` D3D12 renderer (`assets/cues/m1_taipei_60s.json`) with 112-instance dynamic DXR 1.1/1.2 inline `RayQuery`, live CUDA PTX vortex rain/pollen, live Tensor Core `mma.sync` SIREN caustics, live Hardware H.264 MFT round-trip, HUD telemetry, and isolated `canvasAbCompare` A/B split-screen comparison.
- Presentation-locked `IAudioClock::GetPosition` 48 kHz WASAPI synthesizer with 16 ms raised-cosine clickless crossfades and default-endpoint hot-migration.

## [0.1.0 — Milestone M0] — Baseline D3D12 Skeleton & Capability Honesty
### Added
- Zero-SDK `BASE_ONLY` CMake C++20 Windows 11 x64 skeleton, triple-buffered flip-model swapchain, `TimelineClock`, `QualityLadder`, `FallbackOrchestrator`, embedded emergency shader fallback, structured path-redacted logging, and standalone `boo_bench` / `boo_cue_validate` CLI tools.
