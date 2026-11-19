# User & Stage Operator Guide — BOO SAH SAH: The Glass Orchid Protocol

**BOO SAH SAH — The Glass Orchid Protocol** is a six-minute (`360.000 s`), nonviolent Taiwanese-inspired spy-noir demoscene film and honest hardware-orchestra showcase for Windows 11 x64, featuring an optional **Interactive NVIDIA-RTX Showcase & Explore Studio** mode.

There is no gameplay, score, or combat: by default, sit back and watch the deterministic 360-second film, switch between **Family** and **Midnight** editions, inspect each hardware pillar live in side-by-side A/B split-screen, or route a clean 16:9 feed into OBS / Resolume via Spout2. At any moment, press **`F`** (or drag with the right mouse button) to step inside any scene with a 6-DOF free camera, jump to curated macro presets (`F2`–`F7`), orbit the sun and toggle RTXDI practical sconces live, inspect 8 live G-buffer / lighting / ray-heatmap AOVs (`I` / `F9`), drop interactive ray-traced physics marbles (`Y` / `F10` / `M` in Explore), drag the RTX ON / Raster OFF split curtain, and experience **NVIDIA NRD v4.18.0** `REBLUR_DIFFUSE_SPECULAR` motion denoising during flight handing off seamlessly to `4096-SPP` `REFERENCE` path-traced convergence when still.

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

Close BOO SAH SAH before upgrading. Setup rejects a running app or locked files and keeps your existing configuration by default. To restore packaged settings, clear **Keep existing config overrides when upgrading**, or use `--replace-config` for a silent install. Cancel remains available during installation; it waits for the current operation to finish before restoring previous files. No separate Visual C++ runtime installation is needed.

### Direct / CLI Launch
Double-click `boo_sah_sah.exe` or launch from PowerShell:

```powershell
# Default 6-minute Family Edition (1920x1080 Balanced, VSync ON, Captions ON)
.\boo_sah_sah.exe

# Midnight Edition at native 1440p Showcase preset in borderless fullscreen
.\boo_sah_sah.exe --edition midnight --preset showcase --fullscreen

# Clean stage / screening mode (HUD hidden, borderless fullscreen, Spout2 clean output enabled)
.\boo_sah_sah.exe --fullscreen --hide-overlay --enable-spout --spout-name BooSahSah_Clean

# Launch directly into Interactive RTX Explore Studio on Hero Macro Preset 2 (CNC Faucet & Porcelain Basin)
.\boo_sah_sah.exe --preset showcase --explore-preset 2
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
| `1` – `6` | Mode: `1` Full Film, `2` RT Solo, `3` Tensor Solo, `4` NPU comparison, `5` CUDA Solo, `6` Finale Loop |
| `L` | Toggle timeline loop |
| `M` | Mute / Unmute audio |
| `-` / `=` (or `Down` / `Up`) | Master volume `-5%` / `+5%` |
| `E` | Switch **Family** / **Midnight** edition (palette, score voicing, and captions) |
| `K` | Toggle lower-third captions (`--captions` / `--no-captions`; non-medical end card always shown) |
| `Q` | Cycle Quality Preset (`Safe 720p` -> `Balanced 1080p` -> `Showcase 1440p` -> `4K Experimental`) |
| `Tab` / `F1` | Cycle HUD: `Full Telemetry` -> `Compact Strip` -> `A/B Split-Screen` -> `Off (Clean Cinema)` |
| `A` | Cycle the split-screen comparison through RT, CUDA, Tensor, Codec, and the stage director. |
| `X` / `C` / `T` / `U` / `V` | Live failover toggle for **RT** (`X`), **CUDA** (`C`), **Tensor** (`T`), **NPU** (`U`), **Codec** (`V`) |
| `O` | Toggle live **Spout2** clean-feed sender (`BooSahSah_Clean`) |
| `W` | Toggle / **One-Key Privacy Stop** for opt-in UVC webcam (`LIVE CAMERA`) |
| `B` | Run asynchronous standalone numerical benchmark sweep (`BENCH [B]`) |
| `F11` / `Alt+Enter` | Toggle borderless fullscreen |
| `F12` | Save clean 8-bit `.bmp` frame + `.json` timing metadata into `diagnostics/captures/` |
| `Esc` | Clean shutdown |

Click a row in the **Hardware** table to compare that engine at the current playhead. The five tabs below the split-view labels let you change engines while watching the same shot. Drag the divider to inspect an edge or reflection. The × button closes the comparison and returns to the previous HUD. These controls also work while paused.

The NPU row shows where the stage director runs. `LIVE` means the NPU has completed an inference. `CPU` means OpenVINO has run the model on the CPU. `SCRIPT` means a deterministic script supplies the controls. `WARMUP` appears while the director prepares or waits for its first inference, `OFF` when inference is disabled, and `FLT` for a fault. The Full HUD shows completed inferences, the last inference time in milliseconds, and completions in the past second in hertz. The GPU renders both halves. The left half uses the current shot’s direction and the right half uses its scripted lighting. Shenzhen holds its authored lighting, so both halves match there.

Press `4`, click the **[4]** shortcut beside the NPU row, or click the bottom NPU button for the guided sanctuary comparison. Close it with × or `4` to restore your previous playhead and play/pause state. Cue sheets without a sanctuary loop open the director comparison in place.

### Interactive NVIDIA-RTX Explore & Lighting Studio Controls (Optional)

When Explore mode is inactive, playback remains 100% deterministic and identical to the canonical 360-second film.

| Input | Function |
|---|---|
| `F` / Right-Mouse Drag / `F:EXPLOR` Pill | Toggle **6-DOF Interactive Explore Camera**. The film and music keep playing while you fly; `Space` pauses and resumes inside Explore, and a paused, still camera refines into the path-traced reference frame (smooth `0.65 s` cubic blend back to live film camera on exit) |
| `W` `A` `S` `D` + `Q` / `E` (in Explore) | Fly Forward / Left / Back / Right + Down / Up (`Shift` = 3.5x sprint, `Ctrl`/`Alt` = 0.22x macro precision) |
| Mouse Wheel / Gamepad `LT` `RT` | Smooth optical FOV zoom (`14 deg` macro telephoto to `85 deg` wide angle) |
| XInput Gamepad (`Y` button + Sticks) | `Y` toggles Explore; Left Stick moves, Right Stick looks, `LB` cycles AOVs, `RB` spawns physics marbles, `D-Pad Left/Right` cycles Hero Presets |
| `F2` – `F7` (or Studio Bar `[1]..[6]`) | Jump to curated **Hero Showcase Presets** (`1` Dawn Bathroom Wide, `2` CNC Faucet & Aerator Droplet Macro, `3` Backlit `Calathea` & Keepsake Orchid, `4` Hero Glass Orchid Sanctuary, `5` Enamel Cathedral & Mirrors, `6` Botanical Sanctuary & Brass Iris) |
| `Alt` + Left-Mouse Drag / `[` `]` / `SUN` `AZM` Sliders | Orbit live **Sun Elevation (Time-of-Day)** and **Sun Azimuth** (`Golden Dawn` -> `High Day` -> `Midnight`) |
| `J` (or Studio Bar `LAMP` button) | Toggle **RTXDI ReSTIR Practical Sconce & Mirror Emitters** ON / OFF |
| `H` (or Studio Bar `WIPE` + LMB Drag) | Toggle & drag the live **RTX ON / Raster OFF Split-Screen Curtain** anywhere from `5%` to `95%` without resetting converged `SPP` |
| `I` / `F9` (`Shift+I` reset, or Studio Bar `AOV`) | Cycle **8-Mode Live Buffer / AOV Inspection** (`0` Final, `1` Base Color, `2` World Normal, `3` Roughness/Metallic/AO, `4` Direct + Caustics + RTXDI, `5` Indirect Diffuse GI, `6` Reflections & Refraction, `7` DXR Ray Heatmap) |
| `Y` / `F10` / `M` in Explore (`Shift+Y` / `Shift+M` clear, or `MRBL`) | Spawn / clear **Interactive *Marbles RTX* Rigid-Body Physics Spheres** (10 sub-stepped Liuli crystal, celadon jade, cognac-amber, 24k gold, brushed nickel, and Calacatta marble spheres rolling into the porcelain vessel basin and vanity with elastic collisions, DXR refraction, and colored Beer-Lambert caustics; `M` remains audio Mute outside Explore) |
| `F8` (or Studio Bar `RST` button) | Reset interactive Sun, Lamp, AOV, and Marble overrides back to authored scene state |
| `Z` / `Shift+Z` (or Studio Bar `TRK1..4` + `MUSC`) | Cycle the **Built-In 4-Track Original Soundtrack** (`1. Neon Orchid`, `2. Acid Lotus`, `3. Velvet Cipher`, `4. Glass Dragon`) + Procedural RTX Synth (`SYNT`); `Shift+Click` on `TRK1..4` or drag-and-drop `.mp3`/`.wav`/`.flac` imports custom audio |
| Motion (`RBLR`) & Stillness (`NRD`) Denoising | During interactive camera motion, sun scrubbing, or rolling physics marbles, dynamically loaded **NVIDIA NRD v4.18.0 (`REBLUR_DIFFUSE_SPECULAR`)** denoises 1-SPP demodulated diffuse GI and glossy reflections in real time using 3D world motion vectors. Once stationary, it hands off automatically to **`NRD::REFERENCE`** progressive accumulation up to **`4096 SPP`** |

Edition changes crossfade the score's voices. The seven acts now include dedicated enamel cathedral, microbial sanctuary, and dawn alcove geometry. Act cards pair Chinese numerals 一–七 with Roman numbers; the original title lettering also includes 台北.

---

## 4. Truth in Advertising: Understanding Engine Badges

BOO SAH SAH enforces strict separation between hardware capability detection and verified live execution:

- **Emerald `LIVE` (`Accelerated`)**: The physical hardware accelerator compiled, ran, and verified live output on the active frame (`DXR RayQuery` hit verification, `CUDA` PTX particle integration, `Tensor` PTX `mma.sync`, explicit OpenVINO `"NPU"` inference, or Hardware H.264 bitstream round-trip).
- **Cyan `SUB` (`Substituted`)**: An honest fallback is rendering the effect (`SM 6.0 Raster`, `8.2K HLSL Compute Particles`, `Scalar HLSL Field`, OpenVINO `CPU` / `ScriptedDirector`, or `SHADER SIGNAL` 8x8 macroblock quantization). Substitutes are **never** labeled as hardware-accelerated.
- **Amber `DEG` (`Degraded`)**: A hardware path timed out or missed verification (e.g., `--inject-npu-timeout`) and failed over smoothly without stalling the film.
- **Slate `OFF` (`Unavailable` / Standby)**: Optional module idle or hardware absent.

Solid dielectric glass props and marbles evaluate 2-interface Snell refraction with ray-traced 3D internal thickness, Beer-Lambert optical absorption, total internal reflection, and colored shadow-ray caustics, while thin crystal petals and float-glass panes use thin-walled transmission. The raster substitute keeps the film visually coherent, though geometry reflections and refractions differ from DXR; neither path claims unbounded multi-bounce spectral path tracing.

---

## 5. Spout2 VJ Output (OBS Studio & Resolume)

Enable clean GPU texture sharing via `--enable-spout` (or press `[O]` at runtime):

```powershell
.\boo_sah_sah.exe --enable-spout --spout-name BooSahSah_Clean
```

- Publishes an adapter-matched `DXGI_FORMAT_B8G8R8A8_UNORM` shared texture strictly from Pass-3 `CanvasClean` (`16:9` tonemapped sRGB).
- **Zero UI Contamination**: Diagnostic HUD panels, lower-third captions, A/B split-screen overlays, and `LIVE CAMERA` indicators are composited exclusively in Pass 4 onto the local window swapchain and **never** appear in the Spout2 feed.
- A Spout2 receiver (OBS Spout2 Source / Resolume Avenue & Arena) is optional. See the [quality report](quality_upgrade_report.md) for measured lifecycle behavior and limitations.

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

### Automated film review

From the repository root, these optional checks save their results without activating a window:

```powershell
# Both editions, hardware solos, and narrative cards: 68 native 1440p images and four montages
./build/x64-release/boo_integration_tests.exe --film-captures --no-activate --capture-dir diagnostics/film-quality

# 224 stress iterations; receipt saved to diagnostics/film-stress/stress.json
./build/x64-release/boo_integration_tests.exe --film-stress --no-activate --capture-dir diagnostics/film-stress
```

Open `manifest.json` beside the captures for per-frame results. Add `--require-debug-layer` to either command for strict D3D12 warning validation; this fails if Windows Graphics Tools is missing. Without that component, a successful run records warning validation as unavailable. All automated application and integration-test launches must include `--no-activate`.

If graphics fail during playback, the app makes one recovery attempt while keeping its place in the film. The diagnostic option `--inject-device-lost` simulates a fault after GPU work has drained. It tests teardown and recreation, not a physical GPU reset or real driver TDR. For current measurements, hardware details, and limitations, read the [Film Quality & Robustness Report](quality_upgrade_report.md).
