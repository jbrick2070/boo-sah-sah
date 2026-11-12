# BOO SAH SAH — Milestone M1 Verification & Kibitz Hardening Report

**Date**: 2026-11-11  
**Target Host**: Windows 11 (`NT 10.0 Build 26200`), `Intel(R) Core(TM) Ultra 9 275HX` (24 logical threads, 64 GB RAM), `NVIDIA GeForce RTX 5080 Laptop GPU` (Blackwell `SM 12.0`, 16 GB VRAM, `D3D12 Feature Level 12_2`, `Shader Model 6_7`, `DXR Tier_1_2`, CUDA Driver `13.5`, NVENC `13.1`), `Intel(R) AI Boost` NPU.  
**Build Configuration**: `BASE_ONLY=ON` (`x64-base-release` & `x64-base-debug`, MSVC `19.44.35228.0` `/W4 /permissive-`, Windows SDK `10.0.26100.0` `dxc.exe` `-Ges -WX`).

---

## 1. Kibitz 4-Round Adversarial Review Campaign Receipt

Per `/kibitz codex`, the graphical and silicon rendering upgrade specification (`docs/graphical_upgrades_plan.md`) was hardened across a complete 4-round adversarial review (`kibitz-runs/2026-11-11-graphical-upgrades/`, `--driver agy --only codex`, **4 external Codex CLI invocations**, `rc = 0` on all 4 rounds):

| Round | External Reviewer (`codex`) | Key Grounded Upgrades & Fixes Folded Into Locked Plan | Artifact Path |
|---|---|---|---|
| **R1 (Broad Critique)** | `gpt-5.6-terra` (`rc=0`) | Upgraded swapchain to **10-bit `R10G10B10A2_UNORM`** (`Rec.709 G2.2 SDR` + 8-bit fallback), added **waitable frame-latency pacing (`max=2`)**, separated clean `16:9` offscreen rendering (`Passes 1–3`) from swapchain HUD presentation (`Pass 4`), added **3-Mip Dual-Kawase HDR Bloom**, **Fitted ACES + IEC sRGB + IGN Dither**, and **96-instance dynamic D3D12 TLAS + DXR 1.1 Inline `RayQuery`**. | `kibitz-runs/2026-11-11-graphical-upgrades/r1/final.md` |
| **R2 (Pressure-Test)** | `gpt-5.6-terra` (`rc=0`) | Enforced **3-slot generation-tagged GPU readback verification** for live DXR ray-hit counts (`successfully_executed_test_work = true` only after verified GPU hits, never on WARP), DPI-aware Win32 client sizing (`WM_DPICHANGED`), lock-free 3-band audio crossover (`180 Hz` / `2400 Hz`), and headless `CanvasClean` determinism gate. | `kibitz-runs/2026-11-11-graphical-upgrades/r2/final.md` |
| **R3 (Wiring & Interfaces)** | `gpt-5.6-terra` (`rc=0`) | Fixed 14 concrete D3D12/C++ wiring details: pre-`beginFrame` canvas reallocation (`syncPresetBeforeFrame`), 5-point GPU timestamp `recordedMask` auto-completion (`EndQuery` on unused slots so `ResolveQueryData` never triggers D3D12 debug errors), zero-descriptor `CopyBufferRegion` UAV clears, explicit per-mip bloom barriers, and sample-derived `(samplePosition / 800)` IGN dither seed independent of `presentationFrame`. | `kibitz-runs/2026-11-11-graphical-upgrades/r3/final.md` |
| **R4 (Convergence)** | `gpt-5.6-terra` (`rc=0`, `CUT THESE: None`) | Folded final 6 convergence polish items: `try/catch` fault boundary in `FallbackOrchestrator::syncPresetBeforeFrame`, `syncLiveRtStatus` publishing `SAFE_FALLBACK_SHADER_ACTIVE` (`Degraded`) without re-invoking `probeAndInitialize`, saturating `uint64_t` VRAM budget subtraction, generation-tagged 15 ms click-free seek ramp in `AudioEngine::resetPhase`, and live `'X'` RT toggle (`setDisableRt`). | `kibitz-runs/2026-11-11-graphical-upgrades/r4/final.md` |

---

## 2. Compilation, Zero-SDK Dependency Gate & CTest Suites

- **Compiler & Shader Strictness**: Both `x64-base-release` and `x64-base-debug` built all 63 targets (`6` M1 `.cso` shaders + M0 shaders + embedded safe DXIL bytecode + `boo_core.lib` + `boo_sah_sah.exe` + `boo_cue_validate.exe` + `boo_bench.exe` + test runners) with **0 errors and 0 warnings**.
- **PE Import Table Gate (`dumpbin /dependents boo_sah_sah.exe`)**:
  - Linked DLLs: `d3d12.dll`, `dxgi.dll`, `ole32.dll`, `AVRT.dll`, `CFGMGR32.dll`, `MFPlat.DLL`, `USER32.dll`, `GDI32.dll`, `KERNEL32.dll`, and MSVC CRT DLLs only.
  - **Zero hard dependencies** on `nvcuda`, `cudart`, `cublas`, `openvino`, `nvEncodeAPI`, or `Spout`.
- **CTest Results (`x64-base-release`: `2.45 s`; `x64-base-debug`: `3.80 s` — `100%` passed)**:
  1. `BooUnitTests` (`9/9` passed): Includes `ProductionCueSheetsAndManifest` (`m1_taipei_60s.json` 3-scene `2,880,000`-sample validation + mixed-cue rejection) and `M1CameraLetterboxAndFlashSafety` (stateless camera determinism, `[-0.15, +0.15]` iris clamp, `--reduced-flash` exact zeroing, analytical 3-band parity, and `16:9` / `16:10` / `4:3` / `21:9` letterbox/pillarbox geometry).
  2. `BooCueValidation` (`6/6` cue sheets passed): `m1_taipei_60s.json`, `film_360s.json`, `solo_rt.json`, `solo_tensor.json`, `solo_npu.json`, `solo_cuda.json`.
  3. `BooHardwareIntegrationTests` (`5/5` passed): Includes `M1HeadlessDeterminismAndDxrVerification`:
     - Verified non-black high-contrast spatial output across all 3 M1 shots (`5.0s`, `30.0s`, `48.0s`) and distinct inter-shot progression (`meanAbsDiff > 0.015`).
     - Verified flash safety across the `45.0s` (`2,160,000`-sample) Glass Orchid reveal (`|meanLumaDelta| < 0.02` at `0.1s` steps; `< 0.005` at 60 Hz steps under `--reduced-flash`).
     - Verified **exact byte-for-byte seek determinism** (`0` differing bytes across `1280 x 720 x 4 = 3,686,400` bytes between a fresh render at `t = 48.0s` and an instance sought via `resetTo(48.0)` after prior playback).
     - Verified live hardware **DXR 1.1 Inline `RayQuery`** execution on `NVIDIA GeForce RTX 5080 Laptop GPU` (`status == Accelerated`, `successfullyExecutedTestWork == true`, `lastVerifiedRayHits() > 0`, `0.003 < meanAbsDiff(dxr, raster) < 0.35`) and live `setDisableRt(true)` transition back to `Substituted` with exact byte-for-byte raster parity.

---

## 3. Measured Live Swapchain & 4-Pass GPU Timings

All automated GUI smoke runs executed non-interactively with `--no-activate` on the 120 Hz desktop (`WaitableLatency=ON(max=2)`, `10-bit R10G10B10A2_UNORM` Rec.709 G2.2 SDR swapchain):

| Scenario | Resolution / Preset | Active RT Backend | Verified Work | Frames | GPU `p50` (`p95`) | `scene_ms` | `bloom_ms` | `composite_ms` | `present_ms` | Stalls / Faults |
|---|---|---|---|---:|---:|---:|---:|---:|---:|---:|
| **M1 Default (`smoke_normal.json`)** | `1920x1080` (`Balanced`) | `D3D12_DXR_1_2_RayQuery` (`10,980` hits) | `true` (`Accelerated`) | `603` (`5.0s`) | `0.422 ms` (`0.448 ms`) | `0.343 ms` | `0.022 ms` | `0.016 ms` | `0.034 ms` | `0 / 0` |
| **M1 16:10 Letterbox + `--reduced-flash`** | `1280x800` (`Balanced`) | `D3D12_DXR_1_2_RayQuery` (`11,257` hits) | `true` (`Accelerated`) | `363` (`3.0s`) | `0.401 ms` (`0.430 ms`) | `0.345 ms` | `0.021 ms` | `0.017 ms` | `0.015 ms` | `0 / 0` |
| **M1 Showcase (`m1_showcase_1440p.json`)** | `2560x1440` (`Showcase`, 2-bounce) | `D3D12_DXR_1_2_RayQuery` | `true` (`Accelerated`) | `362` (`3.0s`) | `0.458 ms` (`0.483 ms`) | `0.364 ms` | `0.022 ms` | `0.017 ms` | `0.062 ms` | `0 / 0` |
| **M1 4K Swapchain (`m1_4k_experimental.json`)** | `3840x2160` (`4K Experimental`, 2-bounce) | `D3D12_DXR_1_2_RayQuery` | `true` (`Accelerated`) | `363` (`3.0s`) | `0.532 ms` (`0.560 ms`) | `0.394 ms` | `0.024 ms` | `0.016 ms` | `0.114 ms` | `0 / 0` |
| **M1 CPU WARP (`m1_warp_smoke.json`)** | `1280x720` (`--force-warp`) | `D3D12_Raster_Safe_M1` | `false` (`Substituted`) | `57` (`3.0s`) | `52.11 ms` (`58.66 ms`) | `38.50 ms` | `9.08 ms` | `5.02 ms` | `2.30 ms` | `0 / 0` |
| **All-Fallbacks (`smoke_all_fallbacks.json`)** | `1280x720` (`--inject-shader-fail` + silent QPC) | `D3D12_EmbeddedSafeShader` | `false` (`Degraded`) | `362` (`3.0s`) | `0.008 ms` (`0.016 ms`) | `0.000 ms` | `0.000 ms` | `0.000 ms` | `0.000 ms` | `0 / 1` (injected) |
| **M0 Cue Compat (`smoke_m0_compat.json`)** | `1280x720` (`film_360s.json`) | `D3D12_Raster_M0_Baseline` | `false` (`Substituted`) | `302` (`2.5s`) | `0.022 ms` (`0.043 ms`) | `0.000 ms` | `0.000 ms` | `0.000 ms` | `0.000 ms` | `0 / 0` |

---

## 4. Silicon Orchestra Numerical Benchmark Scorecard (`boo_bench.exe`, 30 Timed Iterations)

| Engine | Hardware Accelerated Score (`RTX 5080 Laptop GPU`) | D3D12 HLSL Substitute Score | Hardware Advantage / Notes |
|---|---:|---:|---|
| **RT (`DXR 1.2 RayQuery`)** | **`17,596.14 MRays/s`** (`p50 = 0.472 ms`) | `16,519.55 MRays/s` | `1.07x` faster with true BVH 2-bounce reflections vs analytic spheres |
| **CUDA (`nvcuda.dll` PTX 7.0)** | **`36,464.41 MPart/s`** (`1,166.9 GB/s`) | `42,115.90 MPart/s` | Zero-SDK dynamic Driver API 1M-particle SoA curl-vortex verified |
| **Tensor (`PTX sm_80 FP16 MMA`)** | **`90.95 TFLOPS`** (`p50 = 0.059 ms`) | `10.38 TFLOPS` | **`8.76x` hardware Tensor Core advantage** (exact `256.0f`/`2048.0f` checksums) |
| **NVENC (`Media Foundation H.264`)** | **`366.20 FPS`** (`9.93 Mbps` Annex-B) | `54,920.91 FPS` (`SHADER SIGNAL` art) | In-memory 1080p NV12 -> H.264 bitstream verified with zero disk writes |
