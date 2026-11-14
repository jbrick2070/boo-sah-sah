# Changelog — BOO SAH SAH: The Glass Orchid Protocol

All notable milestones implemented in accordance with `docs/SPEC_v4.md`.

## [1.1.0 — RTX Showcase Pass] — Ray-Traced Lighting & Cinematic Lens
### Added
- **Ray-traced lighting in `m2_mesh.hlsl` (DXR 1.1 inline `RayQuery`)**: stochastic soft key-light shadows (sun/moon disk cone sampling, 90-unit reach, replacing the old 3-unit binary contact test), ray-traced ambient occlusion (cosine-weighted, distance-adaptive radius), roughness-driven glossy reflections with grazing-angle anisotropic stretch (rain-street light streaks on wet asphalt, water and curtain walls), and reflection hits shaded with interpolated vertex normals, key light and (Showcase) a hit-point shadow ray.
- **Ray-marched volumetric light shafts**: each march step fires a BVH visibility ray toward the key light through a height-falling medium with Henyey-Greenstein phase; a new `m2_sky_rt_ps.cso` sky pipeline continues the shafts across silhouettes into open sky.
- **Spectral glass**: separate R/G/B refraction rays (IOR 1.455 / 1.49 / 1.535) for chromatic dispersion through the Glass Orchid, plus thin-film interference iridescence on petal rims.
- **Physically based shading**: GGX / Smith height-correlated specular with per-material metal/dielectric F0 (24k gold, structural steel, obsidian stela, titanium, water), specular occlusion, wrapped-diffuse subsurface on the ruby labellum, back-lit enamel, and a velvet sheen lobe on the Neural Velvet ribbon.
- **Sky and weather**: Rayleigh + Mie aureole, a textured moon disk with Mie halo aligned to the act-2 key light, an anti-aliased procedural star field (kept out of aerial-perspective fog), and procedural rain-impact ripples on puddles and open water.
- **Depth pre-pass** in `TrilogyMeshRenderer` (VS-only PSO, `precise` clip-space math) so the ray-query shading pass runs exactly once per visible pixel; the sky now draws last at the far plane.
- **Cinematic lens composite (`PSCompositeCanvas`)**: autofocus thin-lens bokeh depth of field (24-tap golden-angle gather, foreground-aware, driven by the M2 linear distance target), lateral chromatic aberration, wide four-octave anamorphic streaks, pseudo lens ghosts + chromatic halo (suppressed under reduced-flash), film halation, and luminance-weighted film grain.
- **Ray budget tiers** via 4 new scene root constants (`quality`, `frameSeed`, `sunCone`, `fxStrength`; 24 -> 28 DWORDs): Balanced 1 shadow / 1 AO / 3 shaft steps, Showcase 2 / 1 / 4 plus hit shadows and 3-ray dispersion, 4K Experimental 1 / 1 / 2. Issued-ray telemetry now counts every query per lane.

### Notes
- All stochastic terms hash `(pixel, samplePosition / 800)`; there is no temporal history, so direct seeks still reproduce identical `CanvasClean` frames.
- These shader and pipeline changes were authored without access to a Windows DXC / MSVC toolchain. Run `scripts/run_smoke.ps1` and the timing captures before release; the existing `QualityLadder` downgrade path remains the safety net if Showcase exceeds its frame budget.

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
