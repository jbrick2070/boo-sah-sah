# BOO SAH SAH — Converged M1 Taipei 60-Second Graphical Upgrade Plan (Locked Post-R4)

> Hardened through a 4-round adversarial Kibitz campaign (`r1 -> r2 -> r3 -> r4`, 4 independent local Codex reviews + code-grounded Antigravity driver anchors/judgments in `kibitz-runs/2026-11-11-graphical-upgrades/`).

## [G0] Scope, Cue/Manifest Routing & Per-Frame DXR Truth State Machine
Deliver **Milestone M1 (`docs/SPEC_v4.md` §13 M1)**: a 60-second autonomous camera-directed Taipei -> Enamel Monolith -> Glass Orchid vertical slice in native C++20 / Direct3D 12 (`BASE_ONLY=ON`, zero optional SDKs).

### [G0.1] Cue Sheet, TOML Config, Manifest & Effect Routing
1. **New Cue Sheet (`assets/cues/m1_taipei_60s.json`)**:
   - `schema = 1`, `film_id = "boo-sah-sah-m1-taipei-60s"`, `sample_rate = 48000`, `duration_samples = 2880000` (`60.000s`), `seed = 266203`, `edition = "family"`.
   - Three contiguous `scenes` covering `[0, 2880000)`:
     - `taipei_rain_corridor`: `[0, 960000)` (`0.0s–20.0s`), `visual = "m1_taipei_rain_dxr"`, `fallback_visual = "m1_taipei_rain_raster"`, `preferred_engine = "RT"`, `audio_section = "taipei_rain_rhodes"`, `allow_director_modulation = true`.
     - `enamel_monolith_ascent`: `[960000, 1920000)` (`20.0s–40.0s`), `visual = "m1_enamel_ascent_dxr"`, `fallback_visual = "m1_enamel_ascent_raster"`, `preferred_engine = "RT"`, `audio_section = "enamel_bass_ostinato"`, `allow_director_modulation = true`.
     - `glass_orchid_opening_reveal`: `[1920000, 2880000)` (`40.0s–60.0s`), `visual = "m1_orchid_reveal_dxr"`, `fallback_visual = "m1_orchid_reveal_raster"`, `preferred_engine = "RT"`, `audio_section = "orchid_crystal_reveal"`, `reveal = "opening_scale_reveal"`, `reveal_sample = 2160000` (`45.0s`), `allow_director_modulation = true`.
   - `solo_loops` inside `m1_taipei_60s.json`: `"rt": [1920000, 2880000]`, `"tensor": [0, 960000]`, `"npu": [960000, 1920000]`, `"cuda": [1920000, 2880000]`, `"finale": [1920000, 2880000]`.
2. **TOML, CLI Settings & Manifest Updates**:
   - `include/boo/settings.hpp` & `src/app/settings.cpp`:
     - Default `AppSettings::cuePath = "assets/cues/m1_taipei_60s.json"` and `AppSettings::title = "BOO SAH SAH — The Glass Orchid Protocol [M1 Taipei Vertical Slice]"`.
     - Add `bool reducedFlash = false;` (`display.reduced_flash` in TOML + `--reduced-flash` CLI flag) and `bool legacyM0Visual = false;` (`--legacy-m0-visual` CLI flag).
   - `config/default.toml`: set `[app]` `cue_file = "assets/cues/m1_taipei_60s.json"`, updated M1 `title`, and `[display]` `reduced_flash = false`.
   - `assets/manifest.json`: set `"milestone": "M1"`, add `assets` entries `cue_m1_taipei_60s` (`assets/cues/m1_taipei_60s.json`) and `shader_m1_taipei_slice` (`assets/shaders/m1_taipei_slice.hlsl`), and append the 6 `m1_*` visual IDs to `visual_presets`.
   - Add `"assets/cues/m1_taipei_60s.json"` to `tools/cue_validate/main.cpp` and `tests/unit/unit_main.cpp` (`testProductionCueSheetsAndManifest`).
3. **Cue-to-Effect Routing Contract (`src/app/main.cpp`)**:
   - `bool cueUsesM1Visuals(const CueSheet& cs)` returns `true` iff `!cs.scenes.empty()` and **every** scene in `cs.scenes` has `visualPreset.rfind("m1_", 0) == 0`.
   - `void createAndRegisterShowEffects(...)` routes M1 cue sheets to `M1TaipeiSliceEffect` + `M0TestVisualEffect(true)` safe fallback, and M0 cue sheets (or `--legacy-m0-visual`) to `M0TestVisualEffect(false)` + `M0TestVisualEffect(true)`.

### [G0.2] Per-Frame Renderer-Owned DXR Verification Contract
- `M1TaipeiSliceEffect` owns live `EngineKind::RT` status (`ownsLiveEngineStatus(RT) == true`) and implements a 3-slot generation-tagged GPU verification readback state machine (`{writtenSeq, rayHitCount}`), cold-start warm-up verification in `probeAndInitialize()`, immediate raster substitution on fault/disable, and quarantine recovery at scene cuts/restarts.
- `FallbackOrchestrator` syncs live `RT` status after `initializeEffects()`, `syncPresetBeforeFrame()`, and `renderFrame()` without ever calling `probeAndInitialize()` during status sync, and blocks ad-hoc benchmark overwrites when M1 owns `RT`.

## [G1] D3D12 Swapchain, Frame Pacing, Timestamps & Headless Execution (`D3D12Context`)
- 10-bit `R10G10B10A2_UNORM` SDR swapchain with checked `DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709` support and scoped `R8G8B8A8_UNORM` fallback.
- Waitable swapchain frame-latency handle (`SetMaximumFrameLatency(2)`, non-alertable wait before allocator reset).
- 5-point validated GPU timestamp queries (`totalMs`, `sceneMs`, `bloomMs`, `compositeMs`, `presentPassMs`) exported under `runtime.gpu_pass_ms` and `runtime.swapchain_bits`.
- `executeSingleCommandListAndWait(...)` for cold-start warm-up and deterministic headless readback tests.
- Per-Monitor V2 DPI client-rect sizing (`AdjustWindowRectExForDpi` + `WM_DPICHANGED`).

## [G2] `M1TaipeiSliceEffect` Admission Control & 4-Pass Architecture
- Preset canvas cap (`Safe` `1280x720` raster; `Balanced`/`Showcase`/`4K Experimental` `1920x1080` capped in M1) with overflow-safe per-committed-resource `GetResourceAllocationInfo` admission control.
- Pass 1: 96-instance dynamic TLAS + `CSDxrTaipeiScene` (`cs_6_5` inline `RayQuery` 1-bounce reflections + 3-slot verification counter) OR `PSRasterTaipeiScene` (`ps_6_0` matching 3D Taipei rain corridor, enamel monolith ascent, and Glass Orchid analytic reflections).
- Pass 2: 3-mip RTV Dual-Kawase HDR bloom (`PSBloomDownsample` + additive tent `PSBloomUpsample`) with explicit per-mip resource barriers.
- Pass 3: `PSCompositeCanvas` -> 16:9 `CanvasClean` (`R8G8B8A8_UNORM`, fitted ACES Narkowicz RRT+ODT + IEC sRGB + sample-deterministic IGN dither).
- Pass 4: `PSPresentLetterboxHud` -> Swapchain RTV (16:9 aspect-preserving Catmull-Rom blit + 10-bit/8-bit dither + self-contained HUD with 3-band audio meters).

## [G3] Exact Camera Math, Audio Crossover & Generation-Tagged Phase Sync
- Deterministic `evaluateM1Camera`, `evaluateM1MusicalEnvelope` (`0.0f` under `--reduced-flash`), and `computeLetterboxViewport16x9`.
- 48 kHz 3-band (`180 Hz` / `2400 Hz`) crossover on pre-mute `baseMono` published via even/odd seqlock, 3-section 60s Taipei musical score, and generation-tagged `AudioEngine::resetPhase` with 15 ms click-free ramp on seek/restart/loop wrap.

## [G4] Verification, Visual Regression & Smoke Acceptance Gates
- `BooCueValidation` + `BooUnitTests` + `BooHardwareIntegrationTests` (headless `CanvasClean` determinism, seek byte-for-byte equality, flash slew `< 0.02`, and conditional hardware DXR `RayQuery` hit & runtime-disable verification) + updated `scripts/run_smoke.ps1`.
