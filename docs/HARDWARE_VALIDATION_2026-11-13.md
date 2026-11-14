# Hardware Validation Report — PR #1 (`claude/dazzling-edison-23ncrc`)

- **Date:** 2026-11-13
- **Branch:** `claude/dazzling-edison-23ncrc` (Draft PR #1 merging `codex/film-quality-robustness` + RTX showcase shaders + live audio reactivity)
- **Host Hardware:** Windows 11 (NT 10.0 Build 26200), NVIDIA GeForce RTX 5080 Laptop GPU (16 GB VRAM, Feature Level 12_2, SM 6.7, DXR Tier 1.2, CUDA Driver 13.5, NVENC API 13.1), Intel Core Ultra 9 275HX (24 logical threads, 64 GB RAM), Intel AI Boost NPU (OpenVINO NPU)
- **Toolchain:** Visual Studio 2022 (`MSVC 19.44.35228.0`), Windows SDK `10.0.26100.0` (`dxc.exe` with `-Ges -WX -O3`), CMake 3.28+, Ninja
- **Window Policy:** Every automated GUI launch ran with `--no-activate`.

---

## 1. Build, Smoke & D3D12 Validation

Both `./scripts/run_smoke.ps1 -Preset x64-base-release` and `./scripts/run_smoke.ps1 -Preset x64-base-debug -SkipBenchmarks` passed **100%** (`exit 0`, `0` warnings/errors under `-Ges -WX -O3`, `0` runtime faults) after all fixes and asset manifest refreshes (`python tools/refresh_asset_hashes.py --check`).

### Smoke Pass/Fail Matrix

| Gate | Test / Sub-check | `x64-base-release` | `x64-base-debug` (`-SkipBenchmarks`) | Notes / Key Output |
|---|---|---:|---:|---|
| `[1/8]` | `BooUnitTests` (CTest) | **PASS** (`1.32 s`) | **PASS** (`2.95 s`) | `20/20` unit cases (including `AudioReactiveFollower` bounds, `< 3 Hz` beat cap, `--reduced-flash`, hold decay) |
| `[1/8]` | `BooCueValidation` (CTest) | **PASS** (`0.05 s`) | **PASS** (`0.13 s`) | Cue JSON + `assets/manifest.json` SHA-256 integrity (`29` tracked assets) |
| `[1/8]` | `BooHardwareIntegrationTests` (CTest) | **PASS** (`45.34 s`) | **PASS** (`61.32 s`) | Live `--no-activate` swapchain, RT/Tensor/NPU/CUDA verification, determinism & recovery |
| `[2/8]` | PE Import Audit (`dumpbin /dependents`) | **PASS** | **PASS** | System DLLs only (`KERNEL32`, `USER32`, `GDI32`, `dxgi`, `d3d12`, `MFPlat`, `MFReadWrite`, `AVRT`, `CFGMGR32`) |
| `[3/8]` | Live 360s Full-Film + Spout2 (`1920x1080`) | **PASS** | **PASS** | Release: `113` frames (`p50=2.802 / p95=3.265 ms`); Debug: `112` frames (`p50=2.819 / p95=3.284 ms`); 10-bit `RGB10A2`, RT/Tensor/NPU/CUDA `VerifiedWork=YES` |
| `[4/8]` | 16:10 Letterbox + `--d3d-debug` + `--reduced-flash --reduced-motion` | **PASS** | **PASS** | `1280x800`, `81` frames (Release `p50=2.635 / p95=2.805 ms`; Debug `p50=2.677 / p95=2.836 ms`), `0` faults |
| `[5/8]` | All-Fallbacks Fault Injection + Device-Lost Recovery | **PASS** | **PASS** | Injected fallback `83`/`82` frames (`p50=0.007 ms`); Device-lost recovery `1/1` with Spout (`39`/`11` frames, `0` unrecovered faults) |
| `[6/8]` | Backward Compatibility (`m2_trilogy_180s`, `m1_taipei_60s`) | **PASS** | **PASS** | M2: `65`/`66` frames (`p50=3.092 / 3.095 ms`); M1: `65` frames (`p50=0.696 / 0.697 ms`) |
| `[7/8]` | Solo NPU Isolation Cue (`solo_npu.json`) | **PASS** | **PASS** | `65` frames (`p50=4.479 / 4.472 ms`, `p95=4.735 / 4.695 ms`) |
| `[8/8]` | Ad-Hoc Hardware Benchmarks | **PASS** | *Skipped* | DXR `17,608.70 MRays/s` (`1.08x`), CUDA `34,877.23 MPart/s`, Tensor `91.29 TFLOPS` (`8.76x`), NVENC `367.63 FPS` (`9.93 Mbps` H.264) |

### D3D12 Validation Findings

Windows Graphics Tools (`d3d12SDKLayers.dll`) is not installed on this machine, so `--d3d-debug` logs a clean warning (`d3d_debug_layer_active = false`). Direct inspection of descriptor heaps, barriers, root signatures, and PSO descriptions uncovered one live descriptor-range hazard and verified the other two focus areas clean:

1. **3-Descriptor Graphics SRV Table (`src/render/m1_taipei_slice.cpp`) — FIXED:**
   - **Finding:** Sharing a single 3-SRV `gfxRootSig` (`t0..t2`) across `psoRasterScene`, `psoBloomDown`, `psoBloomUpAdditive`, and `psoCompositeCanvas` caused `psoBloomDown` (which binds `srvGpuAt(3 + m)` while rendering into `bloomMips[m]` at descriptor `4 + m`) to include `bloomMips[m]` inside its bound 3-descriptor SRV range while `bloomMips[m]` was in `D3D12_RESOURCE_STATE_RENDER_TARGET` (a D3D12 debug-layer resource-state hazard on `D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND` tables without `DATA_VOLATILE` / `DESCRIPTORS_VOLATILE`).
   - **Fix:** Kept `gfxRootSig` as a 1-SRV table (`t0`) for `psoRasterScene`, `psoBloomDown`, and `psoBloomUpAdditive`, and added a dedicated 3-SRV `compositeRootSig` (`t0..t2` bound at `srvGpuAt(2)`: `[2]=sceneDistance`, `[3]=sceneHdr`, `[4]=bloomMip0`) exclusively for `psoCompositeCanvas` (when all three resources are in `PIXEL_SHADER_RESOURCE`).
2. **Sky PSO Drawing After Z-Prepass (`src/render/trilogy_renderer.cpp`, `assets/shaders/m2_mesh.hlsl`) — CLEAN:**
   - Both `s.sky` (raster) and `s.skyRt` (DXR) draw after the Z-prepass and opaque passes with `VSFullscreen` outputting `z = 1.0`, `DepthEnable = TRUE`, `DepthWriteMask = ZERO`, `DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL`, and `DSVFormat = DXGI_FORMAT_D32_FLOAT` matching the cleared (`1.0f`) scene depth buffer. Only unoccluded far-plane pixels (`depth == 1.0`) pass depth test.
3. **Render-Target vs PSO Format Mismatches — CLEAN:**
   - Verified every `OMSetRenderTargets` call against `NumRenderTargets` and `RTVFormats[]`: `trilogy_renderer.cpp` binds 2 RTVs (`R16G16B16A16_FLOAT`, `R32_FLOAT`) for `s.opaque`/`s.sky` and 7 RTVs (`hdr`, `dist`, `diffRadHit`, `specRadHit`, `normalRough`, `viewZ`, `motionVec`) matching `makeScenePso(..., 7, ...)` for `s.opaqueRt`/`s.skyRt` (with `PSSky` writing `SV_Target0..6` when `BOOSAH_RT` is defined); `m1_taipei_slice.cpp` binds `R16G16B16A16_FLOAT` for bloom mips and `R10G10B10A2_UNORM` for `canvasClean` and swapchain backbuffers. Zero mismatches.

---

## 2. GPU Frame Timings

Measured on RTX 5080 Laptop GPU (Release, `--no-activate --mute --preset <preset> --start-seconds <s> --duration-seconds 2`, Codec in normal standby, `0` quality downgrades, `0` faults). Values are **`p50 / p95` ms**.

### A. Default 360s Full Film (`assets/cues/film_360s.json`)

| Start (s) | Scene Beat | `showcase` (`2560x1440`) `p50 / p95` (ms) | `4k_experimental` (`3840x2160`) `p50 / p95` (ms) |
|---:|---|---:|---:|
| 15 | Act I — Shanghai rain trestle | `4.466 / 4.607` | `9.740 / 10.230` |
| 45 | Act I — Huangpu river & orchid macro | `6.533 / 6.780` | `13.886 / 14.531` |
| 75 | Act I/II — Rain bridge & skyline | `6.618 / 6.749` | `13.905 / 14.489` |
| 108 | Act II — Nocturnal bay & skyline | `5.128 / 5.418` | `10.537 / 10.980` |
| 168 | Act II — Orchid bloom macro | `7.084 / 7.330` | `14.417 / 14.941` |
| 176 | Act II — Wet rain-terrace wide | `7.958 / 8.114` | `16.443 / 16.886` |
| 200 | Act III — Rain-terrace stem / skyline | `6.057 / 6.335` | `12.479 / 12.865` |
| 260 | Act III — Golden bay aerial shafts | `6.130 / 6.218` | `13.642 / 14.047` |
| 320 | Act III — Taipei 101 nocturnal approach | `7.635 / 7.838` | `15.589 / 16.096` |
| 345 | Epilogue — Dawn bathroom & Taipei vista | `7.899 / 8.144` | `16.862 / 17.806` |

### B. M2 180s Trilogy (`assets/cues/m2_trilogy_180s.json`) — New vs `docs/TRILOGY_PIPELINE.md` Baseline

| Start (s) | `showcase` Old (`p50 / p95` ms) | `showcase` New (`p50 / p95` ms) | `showcase` `p95` Delta | `4k_experimental` Old (`p50 / p95` ms) | `4k_experimental` New (`p50 / p95` ms) | `4k_experimental` `p95` Delta |
|---:|---:|---:|---:|---:|---:|---:|
| 15 | `5.927 / 6.683` | `5.667 / 5.930` | `-0.753 ms` | `12.742 / 13.299` | `13.069 / 13.726` | `+0.427 ms` |
| 45 | `4.580 / 5.073` | `5.188 / 5.394` | `+0.321 ms` | `8.988 / 10.300` | `11.772 / 12.316` | `+2.016 ms` |
| 75 | `4.648 / 5.252` | `5.136 / 5.327` | `+0.075 ms` | `9.172 / 10.632` | `11.421 / 11.912` | `+1.280 ms` |
| 108 | `5.505 / 6.399` | `5.641 / 5.884` | `-0.515 ms` | `11.757 / 12.732` | `12.267 / 12.680` | `-0.052 ms` |
| 168 | `6.299 / 6.450` | `5.578 / 5.821` | `-0.629 ms` | `12.652 / 12.832` | `11.739 / 12.174` | `-0.658 ms` |
| 176 | `4.278 / 4.446` | `3.929 / 4.105` | `-0.341 ms` | `7.796 / 8.160` | `8.025 / 8.230` | `+0.070 ms` |

`showcase` (`2560x1440`) `p95` peaks at **`8.144 ms`** across all 16 scenes (`< 50%` of the `16.6 ms` 60 Hz budget). No `rtLightShafts()` step reduction or distance cull was needed.

---

## 3. Visual Review & Shader Fixes

38 clean `2560x1440` captures (`Family` and `Midnight` across all 10 `film_360s` timestamps, Dawn Finale `342, 345, 352, 355 s`, and all 6 `m2_trilogy_180s` timestamps, plus `+1` frame flicker pairs and AOV-7 ray-count heatmaps) and 18 1:1 inspection crops were generated via `boo_integration_tests.exe --validation-captures --no-activate` into `build/validation_captures/` and `build/validation_captures/crops/`:

- **Full-frame contact sheets:** `build/validation_captures/film360_family_montage.png`, `build/validation_captures/film360_midnight_montage.png`, `build/validation_captures/m2_180_family_montage.png`, `build/validation_captures/m2_180_midnight_montage.png`
- **Ray-count heatmaps (AOV 7):** `build/validation_captures/film360_family_{15,168,176,320,345}s_aov7_rays.png`, `build/validation_captures/m2_180_family_{15,168,176}s_aov7_rays.png`

### Checklist Findings & Applied Fixes

1. **Light-shaft noise or banding (`PASS`):**
   - 3-step IGN-jittered (`interleavedGradientNoise(svPos.xy, frameIdx)`) visibility rays in `rtLightShafts()` exhibit zero contour banding across golden-hour bay (`m2_180_family_75s.png`, `film360_family_260s.png`) and nocturnal beams (`m2_180_family_108s.png`). Minor ~1–2% single-pixel IGN stipple blends naturally under composite film grain without temporal history.
2. **Shafts at silhouettes against the sky (`PASS`):**
   - Because `PSSky` draws at `z = 1.0` after opaque passes and invokes the identical `rtLightShafts(d, 90.0, shaftJitter, queries)` integration as `PSMain` (clamped at `min(dist, 90.0)`), volumetric shafts cross distant tower silhouettes seamlessly without seams or bright/dark halos (`build/validation_captures/crops/crop_shafts_silhouette_m2_75s.png`, `crop_shafts_silhouette_m2_108s.png`, `crop_shafts_silhouette_film_260s.png`).
3. **Stars, sun, moon, or clouds printing on fogged buildings (`FIXED` -> `PASS`):**
   - **Issue found:** `PSMain` previously evaluated `float3 fogSky = sky(norm(i.world - eye))`, which included the sharp `pow(..., 180/260)` solar disc, `tensorField` high-altitude cloud bands, and Qwantani Dawn HDRI detail inside aerial-perspective fog on distant towers (`345s` window towers, `75s`/`260s` sunward skyline). (`sky()` already gated procedural stars on `showFlags == 0u`, which is inactive in `PSMain`.)
   - **Fix (`assets/shaders/m2_mesh.hlsl`):** Factored `skyImpl(d, includeCelestialAndClouds)` and routed `PSMain` aerial fog through `skyAerialFog(d)` (`skyImpl(d, false)`), preserving smooth Rayleigh/Mie horizon haze and broad sun glow while preventing celestial discs, HDRI textures, or high clouds from bleeding onto fogged geometry (`build/validation_captures/crops/crop_dawn_window_towers_345s.png`, `crop_fog_sky_film_15s.png`, `crop_fog_sky_midnight_15s.png`).
4. **DoF focus pumping or haloing around the orchid (`FIXED` -> `PASS`):**
   - **Issue found:** `autofocusDistance()` used 9 integer `Load()` point probes; when a thin orchid stem (`200s`) or moving petal silhouette (`45s`, `168s`) crossed a single pixel tap, `focusDist` stepped discontinuously (~`1.8 m <-> 42 m`). Gather bokeh bilateral depth weighting (`wNear`/`wFar`) already prevented foreground bleed halos.
   - **Fix (`assets/shaders/m1_taipei_slice.hlsl`):** Replaced the 9 integer taps with 13 Gaussian-weighted sub-pixel bilinear inverse-depth (`1/d`) probes (`sampleInvDistanceBilinear`), making `autofocusDistance()` `C^0`-continuous across sub-pixel silhouette motion with zero temporal history (`build/validation_captures/crops/crop_orchid_dof_thinfilm_fam_45s.png`, `crop_orchid_dof_thinfilm_fam_168s.png`, `crop_orchid_dof_thinfilm_m2_168s.png`).
5. **Lens ghosts & chromatic halo strength (`FIXED` -> `PASS`):**
   - **Issues found:** (a) `frac(pin.uv + ghostVec * g)` wrapped off-screen sky/sun highlights modulo `1.0` onto the opposite side of the frame (`m2_180_family_75s.png` harbour water); (b) the chromatic halo ring applied a double radial offset (`huv = pin.uv + haloDir` at radius `0.36` followed by `abs(length(huv - 0.5) - 0.36)`), stamping un-mirrored cyan/orange rings onto dark bathroom cabinetry (`film360_family_345s.png`, `352s`); (c) `0.55`/`0.35` weights without a highlight knee allowed broad overcast/dawn skies to cast coloured bands.
   - **Fix (`assets/shaders/m1_taipei_slice.hlsl`):** Removed `frac()` wrapping in favour of smooth `[0, 1]` UV edge fade, corrected the halo ring to sample true optical-centre reflection `huv = 1.0 - pin.uv` gated at `abs(r - 0.36)`, added a compact highlight luminance knee (`lum > 1.15`), and reduced ghost/halo weights (`0.55 -> 0.24`, `0.35 -> 0.16`) (`build/validation_captures/crops/crop_ghost_halo_m2_75s_water.png`, `crop_ghost_halo_film_345s_vanity.png`, `crop_ghost_halo_film_352s_mirror.png`).
6. **Rain ripples aliasing at distance (`FIXED` -> `PASS`):**
   - **Issue found:** `rainRippleGrad()` used a fixed `38.0` ring sharpness faded only by linear distance (`dist / 55.0`); at shallow grazing angles (`15–45 m` on `176s`, `200s`, `m2_45s`), foreshortened capillary rings compressed below Nyquist into 1-pixel specular sparkles.
   - **Fix (`assets/shaders/m2_mesh.hlsl`):** Computed analytical world-space pixel footprint `pxWorld = dist / (max(abs(nGeom.y) * saturate(dot(nGeom, v)), 0.06) * 900.0)` and passed it to `rainRippleGrad(xz, time, pxWorld)` to broaden ring width (`invWidth`) and fade sub-pixel rings smoothly before Nyquist (`build/validation_captures/crops/crop_rain_ripples_film_176s.png`, `crop_rain_ripples_film_200s.png`, `crop_rain_ripples_m2_45s.png`).
7. **Thin-film colours (`PASS`, treble modulation retuned):**
   - `thinFilmInterference()` is restricted to exterior botanical crystal (`matId == 3, 4`, `0.10 + 0.55 * edgeRim` Fresnel blend) and reads as subtle pearlescent celadon/rose rim sheen in both `Family` and `Midnight` (`crop_orchid_dof_thinfilm_fam_168s.png`, `crop_orchid_dof_thinfilm_mid_168s.png`, `crop_orchid_dof_thinfilm_mid_45s.png`). Reduced audio treble `filmNm` excursion (`110 -> 75 nm`) so sustained hi-hats never push rim hues toward neon magenta/green.
8. **Black, NaN, or flickering pixels (`PASS`):**
   - Automated HDR readback checks across all 38 clean captures verified **`hdr_nonfinite_components == 0`** (`0` NaN/Inf components), `0` pure-black frame dropouts, and smooth frame-to-frame deltas on `+1`-frame pairs (`15s, 168s, 176s, 320s, 345s`).

---

## 4. Audio Reactivity Evaluation & Gain Retuning

### Windowed Listening & Visual Runs

Played the full 360-second film windowed (`--no-activate`) with WASAPI audio active (`AudioSampleClock_48kHz`, `0` faults, `0` quality downgrades) across:
- **Bundled OST tracks** (`assets/music/01_neon_orchid.mp3` -> `Z` soundtrack selector -> `assets/music/02_acid_lotus.mp3`, plus full-track telemetry across `03_velvet_cipher.mp3` and `04_glass_dragon.mp3`): `11,481` frames, GPU `p50=4.172 ms / p95=8.317 ms`.
- **Procedural RTX Synth Score** (Slot `0`, `Family` and `Midnight`): `11,472` frames, GPU `p50=3.941 ms / p95=5.499 ms`.

### Diagnosis Before Retuning

In `AudioSynthesizer48k::evaluateVoiceSample` (`src/audio/engine.cpp`), custom/OST MP3 playback pre-scales `sL/sR` by `kCustomGain = 4.2f` to compensate for the `0.14f` WASAPI master headroom scalar applied only to speaker output. Feeding `outMono = 0.5f * (sL + sR)` directly into the 3-band Linkwitz-Riley crossover (`bandGain = 4.5f`) clipped `raw.low` on **`57.6%–68.2%`** and `raw.mid` on **`67.0%–84.6%`** of frames across all four bundled MP3s. That pegged `state.low`/`state.mid` near `1.0` (`p50 = 0.981–1.000`), froze skyline VU columns near the roof, and starved beat detection (`lowAverage_` rose to `~0.94`, pushing `lowAverage_ * 1.12f + 0.05f > 1.0` so beats only fired `0.46–0.82 /s`).

### Complete Table of Audio-Reactivity Changes

| File | Location / Expression | Old Value | New Value | Rationale |
|---|---|---:|---:|---|
| `src/audio/engine.cpp` | Custom/OST crossover meter tap `outMono` (`line 724`) | `0.5f * (sL + sR)` | `0.08f * (sL + sR)` | Matches custom/OST pre-master tap headroom to procedural voices without altering speaker volume (`outLeft/outRight`); reduces OST raw band clipping from `57.6%–84.6%` to `0.0%–0.2%`. |
| `include/boo/audio_reactive.hpp` | `AudioReactiveFollower::kMinBeatIntervalSeconds` (`line 22`) | `0.34` | `0.36` | Keeps steady-state beat retriggers `< 2.78 /s` (strictly `< 3 Hz` flash-safety ceiling, >= `0.34` limit) while cleanly tracking `100–130 BPM` kicks. |
| `src/audio/audio_reactive.cpp` | Auto-gain running-peak decay tau (`line 43`) | `8.0f` s | `6.0f` s | Faster recovery when tracks drop from dense choruses into sparse ambient breakdowns. |
| `src/audio/audio_reactive.cpp` | Bass onset threshold (`line 56`) | `lowAvg * 1.12f + 0.05f` | `lowAvg * 1.08f + 0.04f` | Catches mastered/bus-compressed OST kick transients reliably (`1.67–2.04 beats/s`) without false triggers in pads. |
| `src/audio/audio_reactive.cpp` | Beat impulse gain (`line 59`) | `0.35f + onset * 3.2f` | `0.30f + onset * 2.8f` | Prevents every moderate kick from saturating `beat` to `1.0`, preserving dynamic accent hierarchy. |
| `src/audio/audio_reactive.cpp` | `low` follower `(attack, release)` (`line 67`) | `(0.030f, 0.22f)` s | `(0.045f, 0.18f)` s | Smooths sub-bass ripple/shaft breathing while letting bass decay cleanly between eighth-note pulses. |
| `src/audio/audio_reactive.cpp` | `mid` follower `(attack, release)` (`line 68`) | `(0.035f, 0.16f)` s | `(0.038f, 0.14f)` s | Tighter midrange phrasing on skyline window columns without frame-to-frame jitter. |
| `src/audio/audio_reactive.cpp` | `high` follower `(attack, release)` (`line 69`) | `(0.025f, 0.11f)` s | `(0.028f, 0.11f)` s | Slight attack damping on sharp hi-hat transients. |
| `src/app/main.cpp` | `audioReactive.update(..., hold)` (`line 2557`) | `Seeking` | `Seeking \|\| Paused \|\| isMuted()` | Decays reactive bands and beats smoothly to zero when playback is paused or muted. |
| `src/render/m1_taipei_slice.cpp` | `allowAudioDrive` & `captureClean` (`lines 3059, 4356`) | `curAov == 0u` | `(curAov == 0u) && !accumulate` + `clean.audioReactive = {}` | Guarantees zero composite audio drive in AOV modes, progressive accumulation, and deterministic captures. |
| `src/render/m1_taipei_slice.cpp` | `pc.bloomIntensity` beat / low pump (`line 3062`) | `+0.20f * kick + 0.08f * low` | `+0.12f * kick + 0.06f * low` | Subtle musical lens bloom breathing instead of noticeable whole-frame brightness pumping. |
| `assets/shaders/m1_taipei_slice.hlsl` | Chromatic aberration kick (`line 1734`) | `1.5 * gAudioKick` | `0.45 * gAudioKick` | Eliminates corner RGB edge-fringe jumping on kicks. |
| `assets/shaders/m1_taipei_slice.hlsl` | Anamorphic streak kick (`line 1752`) | `0.6 * gAudioKick` | `0.40 * gAudioKick` | Gentle cyan streak emphasis on downbeats without overpowering specular framing. |
| `assets/shaders/m2_mesh.hlsl` | Emerald architectural trim `m == 6u` (`line 339`) | `0.34 * audioHigh()` | `0.26 * audioHigh()` | Prevents treble shimmer chatter on distant bridge/tower trims. |
| `assets/shaders/m2_mesh.hlsl` | Skyline VU meter threshold & gain (`lines 381–382`) | `smoothstep(0.0, 0.35, band - flr*0.75)`, `0.16*band + 0.30*meter` | `smoothstep(0.0, 0.20, band - flr*0.92)`, `0.14*band + 0.38*meter` | Uses the full 40-storey facade travel (`0.92`) with readable musical rise/fall across bass, mid, and treble towers. |
| `assets/shaders/m2_mesh.hlsl` | Bioluminescent flora `m == 22u` (`line 394`) | `0.05 * audioBeat()` | `0.042 * audioBeat()` | Balances warm flora beat pulse with ambient sanctuary glow. |
| `assets/shaders/m2_mesh.hlsl` | `rtLightShafts()` bass breathing (`line 997`) | `0.28 * audioLow()` | `0.22 * audioLow()` | Keeps `0.90` base atmospheric shaft presence while softening peak bass swell. |
| `assets/shaders/m2_mesh.hlsl` | Water wave normal `baseRip` (`line 1121`) | `0.45 * audioLow()` | `0.28 * audioLow()` | Prevents reflected skyline towers from wobbling excessively during sustained sub-bass notes. |
| `assets/shaders/m2_mesh.hlsl` | Rain-impact ring `ringAmt` (`line 1129`) | `1.1 * audioLow()` | `0.55 * audioLow()` | Halves bass-driven specular ring flare on wet stone and harbour surfaces. |
| `assets/shaders/m2_mesh.hlsl` | Orchid `nectarGlow` (`line 1693`) | `0.9 * audioBeat()` | `0.65 * audioBeat()` | Warm inner heart pulse inside the Liuli labellum without blowing out delicate refractions. |
| `assets/shaders/m2_mesh.hlsl` | Orchid `thinFilmInterference` `filmNm` (`line 1705`) | `110.0 * audioHigh()` | `75.0 * audioHigh()` | Keeps iridescent crystal petal rims refined during dense percussion passages. |

### Post-Retune Audio Telemetry & Safety Verification

| Track | Raw Low/Mid Clip % (Before -> After) | Follower `low` `p10 / p50 / p90` | Mean Beat Rate | Max Beats in Any 1.0 s Window | `--reduced-flash` Max Beat |
|---|---:|---:|---:|---:|---:|
| Procedural (`Family`, 360 s) | `0.0% / 0.0%` -> `0.0% / 0.0%` | `0.295 / 0.625 / 0.858` | `2.01 /s` (`120.7 BPM`) | `3` (`kMinBeatIntervalSeconds = 0.36`) | `0.000` |
| Procedural (`Midnight`, 360 s) | `0.0% / 0.0%` -> `0.0% / 0.0%` | `0.298 / 0.630 / 0.861` | `2.02 /s` (`121.5 BPM`) | `3` (`kMinBeatIntervalSeconds = 0.36`) | `0.000` |
| `01_neon_orchid.mp3` | `57.6% / 67.0%` -> `0.0% / 0.0%` | `0.321 / 0.642 / 0.856` | `1.67 /s` (`100.0 BPM`) | `3` (`kMinBeatIntervalSeconds = 0.36`) | `0.000` |
| `02_acid_lotus.mp3` | `68.2% / 81.3%` -> `0.2% / 0.1%` | `0.348 / 0.684 / 0.873` | `2.01 /s` (`120.8 BPM`) | `3` (`kMinBeatIntervalSeconds = 0.36`) | `0.000` |
| `03_velvet_cipher.mp3` | `61.4% / 84.6%` -> `0.0% / 0.1%` | `0.335 / 0.665 / 0.864` | `2.04 /s` (`122.6 BPM`) | `3` (`kMinBeatIntervalSeconds = 0.36`) | `0.000` |
| `04_glass_dragon.mp3` | `63.8% / 79.1%` -> `0.1% / 0.1%` | `0.319 / 0.659 / 0.868` | `1.99 /s` (`119.3 BPM`) | `3` (`kMinBeatIntervalSeconds = 0.36`) | `0.000` |

- **Determinism verified:** `boo_integration_tests.exe --trilogy-captures --no-activate` (`M2CapturesAndDeterminism`), `boo_integration_tests.exe --validation-captures --no-activate` (`38/38` `seek_repeat_equal == true`), and `M2LiveNoActivateSwapchainDeterminism` all passed with identical `RGB10A2` SHA-256 hashes across repeated seeks and live audio playback.

---

## 5. Open Issues Chosen Not to Fix

1. **Optional Windows Graphics Tools (`d3d12SDKLayers.dll`) not installed on host:** Live D3D12 debug-layer messages could not be collected at runtime because the OS optional feature requires administrator DISM installation; root signatures, descriptor ranges, barriers, and PSO formats were audited statically and verified across Debug and Release hardware runs.
2. **Pre-existing analytical harbour caustic wave (`assets/shaders/m2_mesh.hlsl`):** Broad daylight water shots (`m2_180_family_75s.png`, `film360_family_260s.png`) show gentle concentric green/amber modulation from the authored `caustic = sin(length(world.xz) * 2.4 - time * 1.8)` + `tensorField` term; left untouched so the established Tensor-driven water look is preserved.
3. **Minor 3-step `rtLightShafts()` IGN stipple (~1–2%):** Visible only under 1:1 magnification in high-contrast shadow-to-sun transitions (`75s`, `260s`); blends into the composite film grain at viewing scale and avoids adding film-mode temporal state or extra DXR ray steps.
4. **`4k_experimental` (`3840x2160`) `p95` slightly above `16.6 ms` at two timestamps:** Peaks at `16.886 ms` (`176 s`) and `17.806 ms` (`345 s`) on this laptop GPU (`p50` remains `<= 16.862 ms`), which is expected for the experimental 4K tier while `showcase` (`2560x1440`) runs at `4.105–8.144 ms` `p95`.
