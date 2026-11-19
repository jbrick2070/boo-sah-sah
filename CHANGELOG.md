# Changelog — BOO SAH SAH: The Glass Orchid Protocol

All notable milestones implemented in accordance with `docs/SPEC_v4.md`.

## [Unreleased] — Hardware comparisons

- Click an engine in the HUD to compare its contribution in the current shot. The five tabs switch between ray tracing, CUDA, Tensor, codec and the NPU director. Close the panel to return to the full frame.
- Press **4** for the guided NPU lighting comparison. The hardware panel identifies Intel NPU, CPU and scripted operation.
- The soundtrack starts with the film clock after graphics warm-up, keeping the opening music aligned with the first scene.

## [Unreleased] — Live Audio Reactivity

### Added
- **`AudioReactiveFollower`** (`include/boo/audio_reactive.hpp`, `src/audio/audio_reactive.cpp`): converts the audio engine's measured low / mid / high band RMS of the real mixed output (procedural score, bundled OST or imported track) into smoothed drive signals with per-band automatic gain, asymmetric attack / release, and low-band onset beat detection. Beat retriggers are capped below 3 Hz; `--reduced-flash` disables the beat and halves / slows the bands. Carried on `FrameContext::audioReactive`.
- **Skyline spectrum analyser**: each tower's lit windows listen to one band (bass, mids or treble, chosen by building hash), with the level climbing the facade like a VU meter.
- **Scene response** in `m2_mesh.hlsl`: jade accents shimmer with treble, the Neural Velvet ribbon swells with mids, micro-flora and the orchid's nectar glow pulse on the beat, light shafts and water chop / rain rings breathe with bass, and the orchid's thin-film colours shift with treble.
- **Lens response**: the beat gently pumps bloom, anamorphic streaks and chromatic fringing in the composite.
- Unit test `AudioReactiveFollowerBeatAndFlashSafety`.
- Integration test `AudioReactiveDriveReachesCanvasDeterministically`. It checks three things:
  - Band and beat drive both change the canvas.
  - A given drive renders identically every time.
  - A later zero-drive render is unaffected, so seek determinism holds.
- Rain-ripple band-limiting now scales with the real canvas height and field of view. It is unchanged at the 1440p reference, keeps resolvable ripple detail at 4K, and filters slightly harder at 720p.
- **24 languages besides English**, following the Windows display language (`--lang <tag>` overrides):
  - Captions, the end card, HUD labels, the camera indicator and window-title words come from string catalogs in `assets/strings`. They are drawn with Windows system fonts through DirectWrite into a text atlas (`include/boo/ui_text.hpp`, `src/render/ui_text.cpp`) that the present pass samples one texel per pixel. Arabic is shaped and laid out right to left. No font is bundled.
  - English keeps the shader glyph tables and is unchanged, apart from the fix below.
  - The Store package description (`packaging/msix/descriptions.json`), the Store listing text (`packaging/store-listing.md`), and the two Windows message texts (`src/app/localization.cpp`) are translated too.
  - Unit tests `UiTextLanguages` and `UiStringCatalogs` (complete catalogs, protected tokens kept, every caption line has an entry, every HUD label fits its box at every HUD size).

### Fixed
- The first row of the LIVE GPU table read `RT[X` (a key hint cut off by the four-character label). It now reads `RT`.
- The capture key (F12) silently saved nothing when the app was started from the Start menu or the Store package, where the working folder cannot be written (`create_directories: Access is denied`). It now falls back to `Pictures\BOO SAH SAH`.
- `packaging/msix/make-msix.ps1` failed on every run: it tried to create an `Assets` folder that the staged app already has.

### Notes
- The drive is packed as RGBA8 into the one spare scene DWORD (root signature size unchanged). It is zero in test / capture contexts, debug AOV modes and progressive accumulation, so deterministic seek captures are unaffected.

## [Unreleased] — RTX Showcase Additions (merged onto Film Quality & Robustness)

### Added
- **Ray-marched volumetric light shafts** in `m2_mesh.hlsl`: three march steps per pixel, each firing a sun/moon visibility ray (`traceShadow`, alpha-cutout aware) through a height-falling exterior haze with Henyey-Greenstein phase. They are added to both the composited and NRD-split (`nonIndirectColor`) outputs, use the existing temporal IGN scheme (stable under REBLUR / progressive accumulation), and are counted in the issued-ray telemetry. Indoor rooms stay clear, matching the aerial-fog rule.
- `PSSky` (RT) carries the shafts into open sky. The sky now draws **after** the Z-prepass and opaque passes at the far plane (`z = 1.0`, `LESS_EQUAL`), so it shades only uncovered pixels and its rays never trace under geometry.
- **Rain-impact ripples** (two decorrelated ring grids) on exterior puddles and open water, with distance fade before they can alias.
- **Thin-film iridescence** on exterior Glass Orchid petals and sepals (single-layer interference sampled at R/G/B wavelengths).
- **Cinematic lens composite** (`PSCompositeCanvas`, film mode only; AOV modes unchanged): autofocus thin-lens bokeh depth of field (24-tap golden-angle, foreground-aware gather from the M2 linear distance target), lateral chromatic aberration, four-octave anamorphic streaks, pseudo lens ghosts + chromatic halo (off under reduced flash), film halation and luminance-weighted grain. The shared graphics root signature's SRV table grows from 2 to 3 descriptors; the composite binds `[2]=sceneDistance, [3]=sceneHdr, [4]=bloomMip0`, and lens flags ride in bits 8–9 of the existing mode word.

### Notes
- All 34 shader rules in `CMakeLists.txt` compile cleanly with DXC 1.8 (`-Ges -WX -O3`, DXIL-validated). The C++ changes were not compiled (no MSVC / Windows SDK in the authoring environment); run `scripts/run_smoke.ps1` and re-measure frame times before release.

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
