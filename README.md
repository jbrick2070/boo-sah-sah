# BOO SAH SAH — The Glass Orchid Protocol

Windows 11 x64, C++20 / Direct3D 12 noninteractive spy-noir demoscene film and honest multi-engine hardware-orchestra showcase (`Milestones M0–M10`).

The default presentation (`assets/cues/film_360s.json`) is the **six-minute (`360.000 s`, `17,280,000` samples @ `48 kHz`) seven-act film** spanning Shanghai, Shenzhen, Taipei, the Enamel Microcosm, and the Botanical Glass Orchid Sanctuary, with **Family** and **Midnight** editions, captions, four standalone Hardware Solos (`RT`, `CUDA`, `Tensor`, `NPU`), clean **Spout2** VJ texture sharing, and strictly opt-in **Signal Hijack** video/webcam compression art. The M2 180-second three-city trilogy (`assets/cues/m2_trilogy_180s.json`) and M1 60-second Taipei vertical slice (`assets/cues/m1_taipei_60s.json`) remain directly playable via `--cue`.

This is pure cinema: there is no gameplay, combat, free camera, or picking.

---

## Build, Smoke Test, and Portable Packaging

Prerequisites: Visual Studio 2022 C++ x64 Build Tools, Windows 10/11 SDK (`10.0.26100.0+` with `dxc.exe`), CMake `3.28+`, and Ninja. `BASE_ONLY` builds require **zero** third-party toolkits (no CUDA, cuBLASLt, OpenVINO, NVENC, or Spout SDKs needed to compile or launch).

```powershell
# Configure, build, and launch Release
./scripts/configure.ps1 -Preset x64-base-release
./scripts/build.ps1 -Preset x64-base-release
./build/x64-release/boo_sah_sah.exe

# Run full automated CTest + PE import audit + multi-mode GUI smoke + benchmarks
./scripts/run_smoke.ps1 -Preset x64-base-release

# Debug verification (requests the D3D12 debug layer when installed)
./scripts/run_smoke.ps1 -Preset x64-base-debug -SkipBenchmarks

# Build single-app native Windows installer (build/dist/BooSahSah-Setup-win64.exe) + portable ZIP
./scripts/package_release.ps1
```

> **One App, One Installer (`BooSahSah-Setup-win64.exe`)**: Running `./scripts/package_release.ps1` packs the unified `boo_sah_sah.exe` application (including `--bench` and `--validate-cues`), compiled DXIL shaders, seven `.bmesh` scenes, cues, configs, and (when `.venv` OpenVINO is present) a compact `openvino\` Intel NPU runtime sidecar into a single LZMS-compressed per-user Windows installer (`%LOCALAPPDATA%\Programs\BooSahSah`, zero UAC elevation required, includes `Uninstall-BooSahSah.exe`). When OpenVINO DLLs are absent, playback uses the deterministic `ScriptedDirector`.

Setup runs extraction on a worker thread and keeps Cancel responsive. Cancellation waits for the current operation before rolling back. Upgrades preserve existing configuration by default; `--replace-config` explicitly installs the packaged defaults. Close the app before upgrading: setup rejects a running app or locked destination files. The application and native installer use the static MSVC runtime. Release and Debug CI build, validate cues, audit optional imports, and exercise packaging, upgrades, rollback, and native uninstallation.

All automated GUI launches must pass `--no-activate` so test windows never steal foreground focus.

### Film quality and robustness gates

```powershell
# 68 native 2560x1440 captures across both editions, hardware solos, and narrative cards
./build/x64-release/boo_integration_tests.exe --film-captures --no-activate --capture-dir diagnostics/film-quality

# 224 lifecycle iterations with memory, audio, and synthetic graphics-recovery receipts
./build/x64-release/boo_integration_tests.exe --film-stress --no-activate --capture-dir diagnostics/film-stress

# Require an active Windows Graphics Tools debug layer; fail if it is unavailable
./build/x64-debug/boo_integration_tests.exe --film-stress --no-activate --require-debug-layer --capture-dir diagnostics/film-stress-debug
```

The capture gate writes 68 clean/narrative PNGs, two additional full-stats layout PNGs, four labeled montages, and `manifest.json`; the stress gate writes `stress.json` under its `--capture-dir`. These are opt-in hardware gates, separate from the short smoke suite. Capture checks cover repeatable seeks, finite HDR output, verified RT status, and clean-feed isolation. The capture director is sample-derived `ScriptedDirector`, so these images do not establish NPU execution.

Debug-layer absence is recorded as unavailable, never as proof of zero warnings. The current RTX 5080 Laptop GPU / Intel AI Boost host lacks Windows Graphics Tools; strict debug validation requires another run with that component installed. See the [Film Quality & Robustness Report](docs/quality_upgrade_report.md) for measured results and remaining limits.

---

## Screening & VJ Controls

| Key | Action |
|---|---|
| `Space` | Play / Pause (`12 ms` clickless gain ramp) |
| `R` / `Home` | Restart current film or solo range |
| `N` / `P` | Next / Previous act |
| `Left` / `Right` | Seek `-2 s` / `+2 s` (`Shift` + `Left`/`Right`: `-10 s` / `+10 s`) |
| `,` / `.` (or `[` / `]`) | Step `-1` / `+1` frame (`800` samples @ `48 kHz`) while paused |
| `G` | Seek to narrative reveal pre-roll |
| `1` – `6` | `1` Full Film, `2` RT Solo, `3` Tensor Solo, `4` NPU Solo, `5` CUDA Solo, `6` Finale Loop |
| `L` | Toggle looping |
| `M` | Toggle mute |
| `-` / `=` (or `Down` / `Up`) | Trim master volume by `5%` (`0–100%`) |
| `Q` | Cycle quality preset (`Safe` -> `Balanced` -> `Showcase` -> `4K Experimental`) |
| `E` | Toggle **Family** / **Midnight** edition |
| `K` | Toggle lower-third captions (`--captions` / `--no-captions`) |
| `X` | Toggle live **DXR RayQuery** / SM 6.0 raster substitute |
| `C` | Toggle live **CUDA PTX** (`65.5K–262K` particles) / `8.2K` HLSL substitute |
| `T` | Toggle live **Tensor Core PTX** `mma.sync` 64-layer SIREN field / scalar HLSL substitute |
| `U` | Toggle live **OpenVINO NPU/CPU** Madame N stage director / `ScriptedDirector` fallback |
| `V` | Toggle live **HW H.264 MFT** Encode+Decode round-trip / `SHADER SIGNAL` / clean bypass |
| `O` | Toggle **Spout2** clean-feed D3D12->D3D11 shared texture sender (`BooSahSah_Clean`) |
| `W` | Toggle / **One-Key Privacy Stop** for opt-in UVC webcam (`LIVE CAMERA`) |
| `A` | Cycle single-variable A/B split-screen pillar (`RT -> CUDA -> Tensor -> Codec`) |
| `Tab` / `F1` | Cycle HUD: `Full Telemetry` -> `Compact Strip` -> `A/B Split` -> `Clean Cinema` |
| `B` | Start asynchronous standalone numerical benchmark sweep (`BENCH [B]`) |
| `F11` / `Alt+Enter` | Toggle borderless fullscreen |
| `F12` | Export clean 8-bit `.bmp` + `.json` timing metadata under `diagnostics/captures/` |
| `Esc` | Clean exit |

---

## Hardware Orchestra Architecture & Truth in Advertising

Capability detection and verified execution are strictly separate across all five engines:

1. **RT (`X`, `solo_rt.json`)**: Dynamic multi-mesh BLAS/TLAS + DXR 1.1/1.2 inline `RayQuery` verified by per-slot GPU hit-counter readback; substitutes matching SM 6.0 raster when absent/disabled.
2. **CUDA (`C`, `solo_cuda.json`)**: Dynamic `nvcuda.dll` PTX `65,536–262,144` GPU-resident 16-substep 3D curl-vortex particles across 5 choreography formations; substitutes `8,192 x 2`-step HLSL compute particles when absent/disabled.
3. **Tensor (`T`, `solo_tensor.json`)**: Dynamic PTX `mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32` 64-layer residual SIREN neural caustic & spectral dome field (`320x180`, `7.55 GFLOP/frame`); substitutes scalar HLSL compute when absent/disabled.
4. **NPU (`U`, `solo_npu.json`)**: Dynamic `openvino_c.dll` explicit `"NPU"` compilation + verified `5 Hz` asynchronous inference of `assets/models/madame_n_director.xml` on physical `Intel(R) AI Boost`, with transparent failover to OpenVINO `"CPU"` and deterministic `ScriptedDirector` (never using unqualified `"AUTO"`).
5. **Codec & Opt-In Media (`V`, `W`)**: In-memory Hardware H.264 MFT encode -> Annex-B verification -> H.264 decode round-trip (`1280x720 NV12`), with optional `--media-input <path>` and `--camera-input` (displaying an always-visible crimson `LIVE CAMERA` indicator when webcam is open) and `SHADER SIGNAL` substitute fallback.
6. **Spout2 Clean Sender (`O`, `--enable-spout`)**: Publishes Pass-3 `CanvasClean` (`16:9`) via LUID-matched shared `BGRA8` texture and `SpoutSenderNames` shared memory—completely free of Pass-4 HUD, captions, and A/B overlays.

The seven-mesh pipeline adds `enamel_cathedral`, `microbial_sanctuary`, and `dawn_alcove` to the four original city/orchid assets. Presentation typography uses 60 original capsule-SDF vector glyphs, including Latin lettering, 台北, and Chinese numerals 一 through 七 paired with Roman act numbers. This is a small title alphabet, not a general Chinese font.

Glass shading uses a single geometry refraction ray with an environment-dispersion approximation. It is not a multi-bounce spectral path tracer. The raster substitute preserves the film's composition and palette but cannot reproduce full DXR geometry reflections. The score integrates modulated oscillator phase and crossfades edition voices to avoid abrupt changes when seeking or switching editions.

Graphics failure triggers at most one teardown/recreation attempt while preserving the sample timeline. `--inject-device-lost` exercises this policy through a synthetic fault after queued GPU work has drained; it does not cause a physical GPU reset or validate recovery from a real driver TDR.

---

## CLI Examples

```powershell
./build/x64-release/boo_sah_sah.exe --help
./build/x64-release/boo_sah_sah.exe --edition midnight --preset showcase --width 2560 --height 1440
./build/x64-release/boo_sah_sah.exe --cue assets/cues/m2_trilogy_180s.json
./build/x64-release/boo_sah_sah.exe --cue assets/cues/solo_npu.json
./build/x64-release/boo_sah_sah.exe --enable-spout --spout-name BooSahSah_Clean --hide-overlay
./build/x64-release/boo_sah_sah.exe --media-input path/to/clip.mp4
./build/x64-release/boo_sah_sah.exe --camera-input
./build/x64-release/boo_sah_sah.exe --reduced-flash --reduced-motion --no-captions
./build/x64-release/boo_sah_sah.exe --force-warp --preset safe
./build/x64-release/boo_sah_sah.exe --validate-cues --no-activate
./build/x64-release/boo_sah_sah.exe --bench --no-activate --bench-iterations 30 --export-bench diagnostics/bench.json --export-bench-csv diagnostics/bench.csv
```

---

## Documentation

- [User & Stage Operator Guide](docs/USER_GUIDE.md)
- [Authoritative v4 Specification](docs/SPEC_v4.md)
- [Architecture Overview](docs/architecture.md)
- [M2 Three-City Trilogy Visual Pipeline](docs/TRILOGY_PIPELINE.md)
- [Engineering Upgrade Report](docs/engineering_upgrade_report.md)
- [Film Quality & Robustness Report](docs/quality_upgrade_report.md)
- [Historical M1 Verification Report](docs/m1_verification_report.md)
- [Changelog (M0–M10)](CHANGELOG.md)
- [Licenses & Asset Provenance](LICENSES.md)
