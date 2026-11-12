# User & Stage Operator Guide — BOO SAH SAH: The Glass Orchid Protocol

**BOO SAH SAH — The Glass Orchid Protocol** is a six-minute (`360.000 s`), noninteractive, nonviolent Taiwanese-inspired spy-noir demoscene film and honest hardware-orchestra showcase for Windows 11 x64.

There is no gameplay, score, combat, free camera, or interactive task—sit back and watch, switch between **Family** and **Midnight** editions, inspect each hardware pillar live in side-by-side A/B split-screen, or route a clean 16:9 feed into OBS / Resolume via Spout2.

---

## 1. System Requirements & Optional Runtimes

- **Minimum (`BASE_ONLY` Safe / Balanced)**:
  - Windows 11 x64 (`22H2` or newer)
  - Any Direct3D 12 Feature Level `11_0+` GPU (discrete or integrated; WARP software rasterizer supported for diagnostics via `--force-warp`)
  - Stereo audio output device (`48 kHz` WASAPI shared mode; automatically switches to a monotonic QPC silence clock if no audio endpoint is connected)
- **Optional Hardware Accelerators (Probed Dynamically at Launch — Zero Hard Dependency)**:
  - **RT (`DXR 1.1 / 1.2`)**: DXR-capable GPU for inline `RayQuery` reflections and thin-shell refraction (substitutes SM 6.0 analytic raster when absent/disabled).
  - **CUDA & Tensor (`nvcuda.dll`)**: NVIDIA RTX GPU with standard Game Ready / Studio driver for `102,400–262,144` PTX curl-vortex particles and `mma.sync.m16n8k16` FP16->FP32 64-layer SIREN neural caustics (substitutes 8.2K HLSL particles & scalar HLSL field when absent/disabled).
  - **Madame N Stage Director (`openvino_c.dll`)**: Intel Core Ultra NPU (`Intel(R) AI Boost`) + OpenVINO 2026.x runtime on `PATH` or project `.venv` for asynchronous `5 Hz` neural mood modulation (falls back to OpenVINO `CPU`, then deterministic `ScriptedDirector` when absent/disabled).
  - **Signal Hijack Codec (`Hardware H.264 MFT`)**: GPU hardware H.264 encoder + decoder MFT for live in-memory `1280x720` compression art (substitutes `SHADER SIGNAL` 8x8 macroblock quantization when absent/disabled).

---

## 2. Installation, Quick Launch & Editions

### Single-File Windows Installer (`BooSahSah-Setup-win64.exe`)
Run `BooSahSah-Setup-win64.exe` (`build\dist\BooSahSah-Setup-win64.exe`) to install **BOO SAH SAH** (`boo_sah_sah.exe`) into `%LOCALAPPDATA%\Programs\BooSahSah` (per-user install, no administrator UAC prompt required, optional Start Menu & Desktop shortcuts, clean uninstallation via Windows **Installed Apps** or `Uninstall-BooSahSah.exe`). Silent installation is also supported (`BooSahSah-Setup-win64.exe --silent`).

### Direct / CLI Launch
Double-click `boo_sah_sah.exe` or launch from PowerShell:

```powershell
# Default 6-minute Family Edition (1920x1080 Balanced, VSync ON, Captions ON)
.\boo_sah_sah.exe

# Midnight Edition at native 1440p Showcase preset in borderless fullscreen
.\boo_sah_sah.exe --edition midnight --preset showcase --fullscreen

# Clean stage / screening mode (HUD hidden, borderless fullscreen, Spout2 clean output enabled)
.\boo_sah_sah.exe --fullscreen --hide-overlay --enable-spout --spout-name BooSahSah_Clean
```

### Selecting Standalone Hardware Solos & Historical Cues
Press keys `1`–`6` during playback to loop individual ranges, or launch dedicated solo cue sheets directly:

```powershell
.\boo_sah_sah.exe --cue assets/cues/solo_rt.json      # M2 Mirror Intelligence (DXR Solo)
.\boo_sah_sah.exe --cue assets/cues/solo_cuda.json    # M3 Particle Mutiny (100K+ CUDA Solo)
.\boo_sah_sah.exe --cue assets/cues/solo_tensor.json  # M4 Neural Velvet (Tensor MMA Solo)
.\boo_sah_sah.exe --cue assets/cues/solo_npu.json     # M5 Madame N (Intel NPU Stage-Director Solo)
.\boo_sah_sah.exe --cue assets/cues/m2_trilogy_180s.json # M2 180s Shanghai-Shenzhen-Taipei Trilogy
.\boo_sah_sah.exe --cue assets/cues/m1_taipei_60s.json   # M1 60s Taipei Vertical Slice
```

---

## 3. Keyboard Transport & Live Inspection Controls

| Key | Function |
|---|---|
| `Space` | Play / Pause (with clickless `12 ms` audio gain ramp) |
| `R` / `Home` | Restart current film or solo loop |
| `N` / `P` | Skip to Next / Previous Act |
| `Left` / `Right` | Seek `-2 s` / `+2 s` (`Shift` + `Left`/`Right`: `-10 s` / `+10 s`) |
| `,` / `.` (or `[` / `]`) | Step `-1` / `+1` frame (`800` samples @ `48 kHz`) while paused |
| `G` | Jump to narrative reveal pre-roll |
| `1` – `6` | Mode: `1` Full Film, `2` RT Solo, `3` Tensor Solo, `4` NPU Solo, `5` CUDA Solo, `6` Finale Loop |
| `L` | Toggle timeline loop |
| `M` | Mute / Unmute audio |
| `-` / `=` (or `Down` / `Up`) | Master volume `-5%` / `+5%` |
| `E` | Switch **Family** / **Midnight** edition (palette, score voicing, and captions) |
| `K` | Toggle lower-third captions (`--captions` / `--no-captions`; non-medical end card always shown) |
| `Q` | Cycle Quality Preset (`Safe 720p` -> `Balanced 1080p` -> `Showcase 1440p` -> `4K Experimental`) |
| `Tab` / `F1` | Cycle HUD: `Full Telemetry` -> `Compact Strip` -> `A/B Split-Screen` -> `Off (Clean Cinema)` |
| `A` | Cycle active A/B Split-Screen pillar (`RT` -> `CUDA` -> `Tensor` -> `Codec`) |
| `X` / `C` / `T` / `U` / `V` | Live failover toggle for **RT** (`X`), **CUDA** (`C`), **Tensor** (`T`), **NPU** (`U`), **Codec** (`V`) |
| `O` | Toggle live **Spout2** clean-feed sender (`BooSahSah_Clean`) |
| `W` | Toggle / **One-Key Privacy Stop** for opt-in UVC webcam (`LIVE CAMERA`) |
| `B` | Run asynchronous standalone numerical benchmark sweep (`BENCH [B]`) |
| `F11` / `Alt+Enter` | Toggle borderless fullscreen |
| `F12` | Save clean 8-bit `.bmp` frame + `.json` timing metadata into `diagnostics/captures/` |
| `Esc` | Clean shutdown |

---

## 4. Truth in Advertising: Understanding Engine Badges

BOO SAH SAH enforces strict separation between hardware capability detection and verified live execution:

- **Emerald `LIVE` (`Accelerated`)**: The physical hardware accelerator compiled, ran, and verified live output on the active frame (`DXR RayQuery` hit verification, `CUDA` PTX particle integration, `Tensor` PTX `mma.sync`, explicit OpenVINO `"NPU"` inference, or Hardware H.264 bitstream round-trip).
- **Cyan `SUB` (`Substituted`)**: An honest fallback is rendering the effect (`SM 6.0 Raster`, `8.2K HLSL Compute Particles`, `Scalar HLSL Field`, OpenVINO `CPU` / `ScriptedDirector`, or `SHADER SIGNAL` 8x8 macroblock quantization). Substitutes are **never** labeled as hardware-accelerated.
- **Amber `DEG` (`Degraded`)**: A hardware path timed out or missed verification (e.g., `--inject-npu-timeout`) and failed over smoothly without stalling the film.
- **Slate `OFF` (`Unavailable` / Standby)**: Optional module idle or hardware absent.

---

## 5. Spout2 VJ Output (OBS Studio & Resolume)

Enable clean GPU texture sharing via `--enable-spout` (or press `[O]` at runtime):

```powershell
.\boo_sah_sah.exe --enable-spout --spout-name BooSahSah_Clean
```

- Publishes an adapter-matched `DXGI_FORMAT_B8G8R8A8_UNORM` shared texture strictly from Pass-3 `CanvasClean` (`16:9` tonemapped sRGB).
- **Zero UI Contamination**: Diagnostic HUD panels, lower-third captions, A/B split-screen overlays, and `LIVE CAMERA` indicators are composited exclusively in Pass 4 onto the local window swapchain and **never** appear in the Spout2 feed.
- If no Spout2 receiver (OBS Spout2 Source / Resolume Avenue & Arena) is open, playback continues with zero blocking or frame drops.

---

## 6. Opt-In Signal Hijack Media & Webcam Privacy

By default, BOO SAH SAH never accesses user media files, cameras, or microphones. You may optionally route a local video clip or UVC webcam into the Signal Hijack compression-art pipeline:

```powershell
# Route a local MP4/WMV video clip into Signal Hijack (loops automatically, audio ignored)
.\boo_sah_sah.exe --media-input "D:\clips\sample.mp4"

# Route default UVC webcam into Signal Hijack (illuminates top-center LIVE CAMERA indicator)
.\boo_sah_sah.exe --camera-input
```

- **Strict Privacy Guarantees**:
  - Audio and microphone streams are unconditionally deselected at the Media Foundation reader boundary.
  - Whenever a webcam is active, a high-contrast crimson **`LIVE CAMERA`** pill is rendered at top-center of the window—even when the HUD overlay is turned off (`Tab` Mode 3). Press **`[W]`** at any moment to stop and release the camera hardware immediately.
  - No video frames or pixels are ever written to disk or transmitted over a network, and all user file paths in logs/diagnostics are redacted (`<USER_HOME>`).
  - Unsupported codecs, corrupt files, or `--inject-media-fail` automatically fall back to procedural Signal Hijack without crashing or pausing the soundtrack.

---

## 7. Accessibility & Diagnostic Export

- **Photosensitivity & Motion Comfort**:
  - `--reduced-flash` (or `reduced_flash = true` in `config/default.toml`) clamps transient lighting pulses to a smooth envelope.
  - `--reduced-motion` (or `reduced_motion = true` in `config/default.toml`) dampens orbital camera excursion (`motion_scale = 0.35`).
  - `[K]` / `--captions` toggles high-contrast lower-third captions for all narrative beats.
- **Diagnostics & Reproducible Benchmarks**:
  - Diagnostic JSON/CSV files are written **only** when explicitly requested via `--export-diagnostics <path>`, `--export-bench <path>`, `--export-bench-csv <path>`, or `boo_bench.exe`.
