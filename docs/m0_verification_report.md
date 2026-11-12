# BOO SAH SAH — Milestone M0 Verification & Silicon Benchmark Report

**Date**: 2026-11-11  
**Milestone Scope**: Milestone M0 Only (*Foundation, CMake, Win32, D3D12 Swapchain, Audio/Silent Clock, Cue Validation, Capability Detection, Graceful Fallback, Diagnostics, Smoke Tests, and Ad-Hoc Numerical Accelerator Benchmarks*)  
**Specification Reference**: `docs/SPEC_v4.md` (Section 13 — Milestone M0 Gate)

---

## 1. Test Machine Hardware & Toolchain Profile

| Component | Detected Specification |
|---|---|
| **OS** | Windows 11 x64 (`NT 10.0 Build 26200`) |
| **CPU** | `Intel(R) Core(TM) Ultra 9 275HX` (24 logical threads, 64 GB RAM) |
| **Primary D3D12 GPU** | `NVIDIA GeForce RTX 5080 Laptop GPU` (Feature Level `12_2`, Shader Model `6_7`, DXR `Tier_1_2`, `16,003 MB` dedicated VRAM) |
| **Secondary D3D12 GPU** | `Intel(R) Graphics` (Feature Level `12_2`, DXR `Tier_1_1`) |
| **Software Fallback GPU** | `Microsoft Basic Render Driver` (WARP, Feature Level `12_1`, DXR `Tier_1_1`) |
| **Detected NPU** | `Intel(R) AI Boost` (`PCI\VEN_8086&DEV_AD1D&SUBSYS_3D6C17AA&REV_01`) |
| **Dynamic System Drivers** | `nvcuda.dll` (CUDA Driver API `13.5`, Blackwell `SM 12.0`), `nvEncodeAPI64.dll` (`NVENC API 13.1`) |
| **Compiler & Build** | MSVC `19.44.35228.0` (C++20, `/W4 /permissive-`), CMake `4.4.0`, Ninja, Windows SDK `10.0.26100.0` (`dxc.exe` `-Ges -WX -O3`) |
| **Optional SDKs Installed** | **None** (Zero CUDA Toolkit, cuBLASLt, OpenVINO SDK, NVENC SDK, or Spout SDK installed on build host) |

---

## 2. Build, Dependency & Automated CTest Results

### 2.1 Compilation (`BASE_ONLY=ON`)
- **`x64-base-release`**: Compiled cleanly (`0` errors, `0` warnings across all C++20 targets and 8 DXC HLSL shader targets).
- **`x64-base-debug`**: Compiled cleanly (`0` errors, `0` warnings across all 61 build steps).

### 2.2 Zero-Optional-SDK PE Import Audit (`dumpbin /dependents boo_sah_sah.exe`)
- **Result**: **PASS**
- Direct imports are strictly inbox Windows 11 & MSVC CRT DLLs: `d3d12.dll`, `dxgi.dll`, `ole32.dll`, `AVRT.dll`, `CFGMGR32.dll`, `MFPlat.DLL`, `USER32.dll`, `GDI32.dll`, `KERNEL32.dll`, and `MSVCP140`/`VCRUNTIME140`/UCRT.
- Zero hard link dependencies on `nvcuda.dll`, `cudart*.dll`, `cublasLt*.dll`, `openvino*.dll`, `nvEncodeAPI64.dll`, or `Spout*.dll`.

### 2.3 Automated CTest Suite (`ctest --output-on-failure`)
- **`BooUnitTests` (`boo_unit_tests.exe`)**: **PASS** (`0.03s` Release / `0.05s` Debug) — 8/8 suites covering `TimelineClock` audio & QPC progression, pause/seek/solo loops, valid/invalid JSON cue partitions (gaps, overlaps, duration mismatches), `QualityLadder` hysteresis & scene-boundary recovery, `FallbackOrchestrator` primary->safe effect failover, `ScriptedDirector` determinism, and CLI/TOML settings parsing.
- **`BooCueValidation` (`boo_cue_validate.exe`)**: **PASS** (`0.03s`) — Validated `film_360s.json` (`17,280,000` samples @ `48,000 Hz`), `solo_rt.json`, `solo_tensor.json`, `solo_npu.json`, `solo_cuda.json`, and `assets/manifest.json`.
- **`BooHardwareIntegrationTests` (`boo_integration_tests.exe`)**: **PASS** (`1.80s` Release / `2.34s` Debug) — Verified live capability truth-in-advertising invariants, headless D3D12 + WARP initialization, silent audio failover (`--inject-audio-fail`), and real hardware benchmark verification.

---

## 3. Runtime Smoke & Fault-Injection Results

| Test Scenario | Clock Source | Active Visual | Frames Rendered | GPU `p50` / `p95` | Peak VRAM | Result |
|---|---|---|---|---|---|---|
| **Normal Release Smoke (`5.0s`, 1280x720, VSync)** | `AudioSampleClock_48kHz` | `M0_GlassOrchidTestVisual` | `604` (`240,000` samples) | `0.023 ms` / `0.027 ms` | `25.7 MiB` | **PASS** |
| **All-Fallbacks Fault Injection (`3.0s`, `--force-silent-clock --inject-shader-fail --disable-*`)** | `SilentMonotonicClock_QPC` | `M0_SafeFallbackVisual` (Embedded DXIL) | `364` (`143,874` samples) | `0.009 ms` / `0.009 ms` | `25.6 MiB` | **PASS** |
| **Live Async HUD Benchmark (`5.0s`, `--bench-on-start`)** | `AudioSampleClock_48kHz` | `M0_GlassOrchidTestVisual` + Live HUD Bars | `602` (`240,000` samples) | `0.061 ms` / `0.063 ms` | `287.1 MiB` | **PASS** |
| **Microsoft WARP CPU Software D3D12 (`2.0s`, `--force-warp`)** | `AudioSampleClock_48kHz` | `M0_GlassOrchidTestVisual` | `233` (`96,000` samples) | `2.166 ms` / `2.448 ms` | `12.0 MiB` | **PASS** |
| **10-Minute Stability Gate (`600.0s`, `--loop`)** | `AudioSampleClock_48kHz` | `M0_GlassOrchidTestVisual` | `72,005` (`11,520,000` wrapped sample = `600.0s` exact) | `0.022 ms` / `0.023 ms` (`p99 = 0.024 ms`) | `25.7 MiB` (`26,984,448 B` — zero growth vs `5.0s` run) | **PASS** (`0` stalls, `0` faults, `0` downgrades) |

---

## 4. Measured Ad-Hoc Numerical Accelerator Benchmarks (RTX 5080 Laptop GPU)

Executed via `boo_bench.exe --iterations 30` (and live in-app via `B` / `--bench-on-start`) with zero external SDKs installed:

| Category | Workload | Backend | Status | Primary Score | Secondary Score | `p50` / `p95` | Output Verification |
|---|---|---|---|---|---|---|---|
| **RT (Hardware)** | 64-Crystal Glass Orchid Lattice (`8.29M` rays/pass, 4 rays/px) | `D3D12 DXR Tier 1.2 (RayQuery BVH)` | `Accelerated` | **`17,629.06 MRays/s`** | `2,125.42 FPS` (1080p) | `0.470 ms` / `0.675 ms` | BVH triangle intersections verified (`88,236` sampled hits) |
| **RT (Substitute)** | Brute-Force 64-Crystal Analytic Ray-Sphere (4 rays/px) | `D3D12 SM 6.0 Compute (Substitute)` | `Substituted` | `31,064.24 MRays/s` | `3,745.21 FPS` (1080p) | `0.267 ms` / `0.271 ms` | Analytic sphere intersections verified (`123,949` sampled hits) |
| **CUDA (Hardware)** | `1,000,000`-Particle Curl-Vortex SoA (`8` substeps, `64 MB` RW/pass) | `CUDA Driver PTX (RTX 5080 SM 12.0)` | `Accelerated` | **`37,202.38 MPart/s`** | `2,380.95 GB/s` | `0.027 ms` / `0.028 ms` | Readback particle age & curl displacement verified (`p0.w = 0.6000s`) |
| **CUDA (Substitute)** | `1,000,000`-Particle HLSL Compute Fallback | `D3D12 SM 6.0 Compute (Substitute)` | `Substituted` | `42,172.74 MPart/s` | `2,699.06 GB/s` | `0.024 ms` / `0.024 ms` | HLSL UAV readback verified (`p0.w = 0.6000s`) |
| **Tensor (Hardware)** | Warp Matrix Multiply-Accumulate (`mma.sync.aligned.m16n8k16.f32.f16.f16.f32`) | `PTX mma.sync.m16n8k16 (SM 12.0)` | `Accelerated` | **`78.77 TFLOPS`** | `6,182.58 MLP FPS` | `6.543 ms` / `7.243 ms` | Exact FP16->FP32 matrix checksums passed (`256.0` & `2048.0`) |
| **Tensor (Substitute)** | Procedural Neural Velvet `960x540` 16-Layer HLSL MLP | `D3D12 SM 6.0 Compute (Substitute)` | `Substituted` | `10.45 TFLOPS` | `39,357.68 FPS` | `0.025 ms` / `0.026 ms` | `960x540` FP16 UAV texture pass verified (**Tensor HW is `7.54x` higher TFLOPS**) |
| **NVENC (Hardware, Optional)** | In-Memory `1920x1080` NV12 -> H.264 Annex-B Encode | `NVIDIA H.264 Encoder MFT [NVENC API 13.1]` | `Accelerated` | **`394.90 FPS`** | `9.93 Mbps` | `2.537 ms` / `3.030 ms` | Annex-B `00 00 00 01` NAL bitstream verified in RAM (`66` frames -> `1,332 KiB`, `0` disk writes) |
| **NVENC (Substitute)** | Uncompressed `1920x1080` YCoCg Macroblock Art Shader | `D3D12 SM 6.0 SHADER SIGNAL (Substitute)` | `Substituted` | `59,523.81 FPS` | `123.43 GPix/s` | `0.017 ms` / `0.017 ms` | 1080p macroblock shader pass verified (art substitute, no bitstream compression) |

---

## 5. Honest Disclosure: Untested Paths on This Host & M1 Next Steps

1. **OpenVINO NPU Inference Execution (`Substituted` on this host)**:
   - Hardware enumeration via Windows PnP (`CM_Get_Device_ID_ListW` on `GUID_DEVCLASS_COMPUTEACCELERATOR`) **detected `Intel(R) AI Boost` (`VEN_8086&DEV_AD1D`)** on this laptop.
   - Because OpenVINO Runtime (`openvino_c.dll`) is not installed on this machine, `successfully_executed_test_work` is honestly reported as `false` (`OPENVINO_RUNTIME_ABSENT`) and `ScriptedDirector` serves deterministic mood vectors.
2. **cuBLASLt SDK Library Path**:
   - The optional CUDA Toolkit (`cublasLt64_*.dll`) is not installed; Tensor Core hardware execution was verified directly via dynamic `nvcuda.dll` Driver API + PTX `sm_80` `mma.sync.m16n8k16` instructions instead.
3. **Remaining for Milestone M1+ (Out of Scope for M0)**:
   - Full scene graph, render-target pool, multi-pass post-processing composer, and Act I–VI production visuals (M1–M8).
   - Shared D3D12<->CUDA external memory (`cuImportExternalMemory`) zero-copy interop for live Act IV rendering (M5).
   - OpenVINO `NPU` ONNX model compilation and async `InferRequest` worker (M3).
   - Optional live media ingest (WASAPI loopback, Spout2, webcam) and Signal Hijack loopback decode (M7/M9).
