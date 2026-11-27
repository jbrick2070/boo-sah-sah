# User & Stage Operator Guide — BOO SAH SAH: The Glass Orchid Protocol

**BOO SAH SAH — The Glass Orchid Protocol** is a six-minute (`360.000 s`), nonviolent Taiwanese-inspired spy-noir demoscene film and honest hardware-orchestra showcase for Windows 11 x64, featuring an optional **Interactive NVIDIA-RTX Showcase & Explore Studio** mode.

There is no gameplay, score, or combat: by default, sit back and watch the deterministic 360-second film, switch between **Family** and **Midnight** editions, inspect each hardware pillar live in side-by-side A/B split-screen, or route a clean 16:9 feed into OBS / Resolume via Spout2. At any moment, press **`F`** (or drag with the right mouse button) to step inside any scene with a 6-DOF free camera, jump to curated macro presets (`F2`–`F7`), orbit the sun and toggle RTXDI practical sconces live, inspect 8 live G-buffer / lighting / ray-heatmap AOVs (`I` / `F9`), drop interactive ray-traced physics marbles (`Y` / `F10` / `M` in Explore), drag the RTX ON / Raster OFF split curtain, and experience **NVIDIA NRD v4.18.0** `REBLUR_DIFFUSE_SPECULAR` motion denoising during flight handing off seamlessly to `4096-SPP` `REFERENCE` path-traced convergence when still.

---

## 1. System Requirements & Optional Runtimes

- **Minimum (`BASE_ONLY` Safe / Balanced)**:
  - Windows 11 x64 (`22H2` or newer)
- Any Direct3D 12 Feature Level `11_0+` GPU with Shader Model 6.0 (discrete or integrated).
- On Microsoft WARP, the app displays a lightweight procedural orchid with a numerical HUD. It follows the film clock and selected edition, changes with each act, and supports reduced motion and reduced flash. Use `--force-warp` to test this software-rendered diagnostic mode.
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
| `7` | Cycle effects detail from low to normal to high. The title bar shows levels 1–3. Safe uses level 1. |
| `1` – `6` | Mode: `1` Full Film, `2` RT Solo, `3` Tensor Solo, `4` NPU comparison, `5` CUDA Solo, `6` Finale Loop |
| `L` | Toggle timeline loop |
| `M` | Mute / Unmute audio |
| `-` / `=` (or `Down` / `Up`) | Master volume `-5%` / `+5%` |
| `E` | Switch **Family** / **Midnight** edition (palette, score voicing, and captions) |
| `K` | Toggle lower-third captions (`--captions` / `--no-captions`; non-medical end card always shown) |
| `Q` | Cycle Quality Preset, outside Explore (`Safe 720p` -> `Balanced 1080p` -> `Showcase 1440p` -> `4K Experimental`); after a light start on integrated graphics, ray tracing stays off |
| `Tab` / `F1` | Cycle HUD: `Full Telemetry` -> `Compact Strip` -> `A/B Split-Screen` -> `Off (Clean Cinema)` |
| `A` | Cycle the split-screen comparison through RT, CUDA, Tensor, Codec, and the stage director. |
| `X` / `C` / `T` / `U` / `V` | Live failover toggle for **RT** (`X`), **CUDA** (`C`), **Tensor** (`T`), **NPU** (`U`), **Codec** (`V`) |
| `O` | Toggle live **Spout2** clean-feed sender (`BooSahSah_Clean`) |
| `W` | Toggle / **One-Key Privacy Stop** (outside Explore) for opt-in UVC webcam (`LIVE CAMERA`) |
| `B` | Run asynchronous standalone numerical benchmark sweep (`BENCH [B]`) |
| `` ` `` | Cycle the dashboard: full table, mini strip, hidden tab |
| `Shift+N` | Neural rendering on / off |
| `Shift+Up` / `Shift+Down` | GPU workout level up / down (adds or removes birds) |
| `Shift+F` | Next flock formation (outside Explore) |
| `Shift+B` | Bird camera on / off |
| `F11` / `Alt+Enter` | Full screen on / off (the same as the **Full screen** button, top right) |
| `F12` | Save a clean 8-bit `.bmp` film frame (without the camera layer) + `.json` timing metadata into `diagnostics/captures/`, or `Pictures\BOO SAH SAH` when that folder cannot be written (Store installs) |
| `Ctrl+L` | Open a Demo-Verse world address from Film or Explore |
| `Esc` | Leave full screen. From a window, clean shutdown |

When the full film reaches its end, the music fades out over its last 8 seconds, and the transport bar's PLAY and NEXT buttons become one **PLAY AGAIN** button (the studio dock's NEXT SCN button reads the same). Click either, or press `Space` or `R`, to start again from the beginning. With looping on (`L`) the film runs straight on instead.

### Watch, roam and visit another world

The film follows its authored camera. Press `F` to explore with your own camera and press `F` again to return to the film. From either view, press `Ctrl+L` to visit another creator’s demo. Enter the public address supplied by its host, in the form `https://host.example/worlds/<uuid>`. Check the destination, then choose the registered Demo-Verse visitor or open the landing page in your browser. Cancel keeps you in the film. The destination host controls entry to its world.

### Full screen

The **Full screen** button sits in the top-right corner. Click it, or press `F11` or `Alt+Enter`, and the picture takes the whole display with no border, title bar or taskbar. The button then reads **Exit full screen** with an `ESC` key cap beside it: click it, or press `Esc`, to come back to the window. `Esc` never closes the app while it is full screen; it only does that from a window.

The button is always there while the HUD is showing. With the HUD off (`Tab` to Clean Cinema) it stays out of the picture, and comes back for three seconds whenever the mouse moves and for five seconds on entering full screen, so there is always a visible way out.

Full screen is a borderless window that exactly covers the display, presented through a flip-model swap chain. On Windows 10 and 11 that lets the GPU scan the app's frames straight out to the display without the desktop compositor in between, which is what exclusive full screen used to be for, and it switches in and out without the display blanking. `--fullscreen` starts that way.

### Integrated graphics

On a PC whose graphics share system memory (integrated graphics), the app starts lighter: the Safe preset (1280x720), raster lighting, and neither ray tracing nor the neural rendering suite is built, so the first start is quick and playback smooth. Naming a preset with `--preset` (even `safe`) turns the light start off: the app then builds everything the chip supports, and a first start takes much longer.

### Loading bar

While the app starts, a bar under **Loading** shows how far it has got. It counts bytes, not seconds. It is the average of four gauges, each read from the machine, and the line under the bar gives their figures:

- **Files.** Megabytes of meshes, textures, shaders and network weights read into memory, out of the total (`268 / 268 MB`). Every file the asset manifest lists is also checked against its SHA-256 as it loads, on a second thread so it costs no start-up time; `SHA-256 verified 264 MB` is how much has matched. A file that does not match is counted as `mismatched` and named in the log.
- **RAM.** System memory the app has committed, against what the last start reached.
- **GPU.** GPU memory the app has in use, as Windows reports it, against what the last start reached.
- **Shaders.** Megabytes of shader code the graphics driver has compiled into pipelines, against what the last start compiled.

The bar moves only when something has actually loaded, never runs backwards, and fills when the first frame is on screen. The totals are this machine's own: the app keeps a small file, `%LOCALAPPDATA%\BOO SAH SAH\startup-memory.txt`, listing what the last start loaded, how much memory it reached, how much shader code it compiled and which build it was, and nothing else. On a first start there is nothing to compare with yet, so the bar follows the files and shaders alone and stops short of full until the first frame. The log records the totals of every start, and `--log-level debug` records the gauges at each stage and how long each stage took.

Beside **Loading** the screen shows the time since start-up began, and under the figures it lists the steps: when each one began, what it is (starting the graphics device, reading files, compiling shaders or preparing the scene, with the exact file or pipeline name) and how long it has taken. Steps still running are bright and their seconds count up; finished ones stay on the list, dimmed, if they took a noticeable time. On the first start of a version a line says why that start is the longest. Start-up's driver calls run off the window's thread, so the screen keeps repainting and the window can be moved or closed however long the call takes. `--log-level debug` writes every step and its time to the log.

The first start after an app or driver update takes longer than the others (about 27 s instead of 7 on an RTX 5080 Laptop GPU), because the driver's shader cache starts empty and it has to compile the app's shaders again. Most of that is the two ray-traced city shaders, about 15 s each; they, the dashboard shaders and the neural rendering shaders compile side by side while everything else loads, and the window keeps answering while start-up waits for them. Later starts take their pipelines from the cache in a fraction of a second. `--cold-shader-cache` starts as after an update (it needs Windows Developer Mode). Closing the app after such a start, the sound and window go at once, but the process may stay in Task Manager for a few seconds while the driver finishes its own shader work, which is what makes the next start fast.

### Interactive NVIDIA-RTX Explore & Lighting Studio Controls (Optional)

Effects detail changes particle density and the number of recursive folds on the Neural Velvet arches. High adds window blinds to nearby city façades. Set the startup level with `--effects-detail high` or `display.effects_detail` in the configuration. In the Shenzhen sequence the film plays at the selected rendering resolution. Telemetry shows the intercepted broadcast in an inset. Use the Codec A/B view to compare it with the native image.

When Explore mode is inactive, playback remains 100% deterministic and identical to the canonical 360-second film.

| Input | Function |
|---|---|
| `F` / Right-Mouse Drag / `F:EXPLOR` Pill | Toggle **6-DOF Interactive Explore Camera**. The film and music keep playing while you fly; `Space` pauses and resumes inside Explore, and a paused, still camera refines into the path-traced reference frame (smooth `0.65 s` cubic blend back to live film camera on exit) |
| `W` `A` `S` `D` + `Q` / `E` (in Explore) | Fly Forward / Left / Back / Right + Down / Up (`Shift` = 2.85x sprint, `Alt` = 0.22x precision; `Page Down` or `Ctrl` also fly down, `Page Up` up) |
| Mouse Wheel; Gamepad `LT` / `RT` | The wheel zooms the field of view (about `17 deg` to `92 deg`); the triggers fly down / up |
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

The four bundled tracks are lossless FLAC recordings from Jeffrey A Brick’s original WAV masters. During playback, the decoded stereo passes directly through the volume control. Track changes use a short crossfade.

Edition changes crossfade the score's voices. The seven acts now include dedicated enamel cathedral, microbial sanctuary, and dawn alcove geometry. Act cards pair Chinese numerals 一–七 with Roman numbers; the original title lettering also includes 台北.

---

## 4. Reading the hardware dashboard

The dashboard has three sizes. Press `` ` `` (backtick) to cycle through them, or use the buttons on the panel:

- **Full:** the complete table. Click **[-]** at the top right to collapse it.
- **Mini:** a four-line strip. The first line colours each engine and graphics feature by state. The second shows the neural suite, Tensor or shader cores, TFLOPS and render FPS; the third the GPU sensors; the fourth the CPU and the workout level. Click **[+]** to expand it or **[X]** to hide it.
- **Hidden:** only a small **HUD +** tab remains. Click it to bring the full table back.

`--dashboard full|mini|hidden`, or `dashboard` under `[display]`, sets the starting size.

The **Real-time graphics** section sits beneath the five hardware rows in the upper-left corner of the Full and Compact HUD. Each row pairs the selected setting with the runtime state. Click a row to change the setting during a shot.

| Dashboard row | Shortcut | What to watch |
| --- | --- | --- |
| Path Tracing | `8` | Low, Medium and High trace one, two or four indirect-light paths per shaded pixel in eligible DXR scenes. |
| DLSS Upscaling | `9` | Quality, Balanced and Performance use progressively fewer scene pixels. DLSS Super Resolution performs the upscale by itself. When Ray Reconstruction is live, one NVIDIA pass handles both reconstruction and upscaling. |
| Ray Reconstruction | `0` | With DLSS enabled, NVIDIA reconstructs the ray-traced image from lighting, normal, material and motion data. |
| Frame Generation | `Shift+9` | Cycles Off, Auto and supported multipliers from 2× through 6×. Auto targets the display refresh rate when the driver supports dynamic generation and uses 2× otherwise. Reflex must be enabled. |
| Reflex | `Shift+8` | On or Boost requests NVIDIA low-latency scheduling of the application's frames. |

`LIVE` indicates that the runtime is doing the work. `IDLE` indicates that the feature is waiting for an eligible scene or presentation condition, `OFF` reflects your choice, and `N/A` indicates unavailable support. A fixed multiplier above the GPU limit appears as a pair, such as `6X/2X` for a 6× request capped at 2×. The full panel also shows Auto's measured multiplier when active. Render FPS counts frames the engine produces. Display FPS counts frames presented to Windows, including frames from Frame Generation. With VSync off, that number can exceed the monitor’s refresh rate. The NPU has its own inference rate in hertz.

### Neural rendering

The **Neural rendering** section sits beneath Real-time graphics. It runs three small neural networks on the GPU during every frame. The suite is off by default, so the film looks the same unless you switch it on. Press `Shift+N` or click the **Neural rendering** heading to switch the whole suite on or off. Click a network's row to switch that network on or off. Click its badge, or Shift-click the row, to solo it. Click the badge again to leave the solo view.

| Row | What it does | Solo view |
| --- | --- | --- |
| Neural Shading | A 16→32→32→3 network grades every HDR pixel. It was trained offline to reproduce an analytic film grade that includes dye crosstalk, shadow tint, highlight shoulder, distance haze and vignette. This is the only network that changes the normal picture. | The left half shows the original frame and the right half shows the neural grade. |
| Neural Textures | A 64×64 grid of 8-bit latents and a small decoder network rebuild a 512×512 crop of the film's Glass Orchid every frame. The latents and decoder take about 26× less memory than the RGBA8 image. | Three framed panels show the original, the neural decode with its quality and compression, and the error at 4× in the film's orchid palette. |
| Radiance Cache | A network over a multi-resolution feature grid trains on 16,384 live scene samples per frame and then predicts radiance for every pixel at half resolution. | The left half shows the live frame and the right half shows what the cache has learned. You can watch it relearn after each cut. |

Each row shows GPU milliseconds measured with timestamps, plus a quality score in dB. For Neural Shading the score compares the network with the analytic grade. For Neural Textures it compares the decode with the original texture. For Radiance Cache it measures how closely the network fits the live frame. The heading shows the combined throughput in TFLOPS. `--export-diagnostics` writes the same figures under `runtime.neural_rendering`.

All three networks run as standard Shader Model 6.0 compute shaders using the shader cores, on GPUs that can read typed images in compute shaders. A light start on integrated graphics leaves them out, and their rows read N/A. Test-rig builds can also run the network inference on **Tensor Cores** through Shader Model 6.10 LinAlg in FP16. The heading shows `TC` when Tensor Cores do the work and `SC` when shader cores do. Shift-click the heading to switch between them and compare the timings. On an RTX 5080 Laptop GPU at 1080p, neural shading took 1.43 ms on shader cores and 0.25 ms on Tensor Cores at the same quality. Cache training stays on shader cores in FP32.

Tensor Cores need a test-rig build and Windows Developer Mode, because Shader Model 6.10 is still a Microsoft preview:

```bash
python tools/fetch_neural_sdk.py
cmake -S . -B build/x64-testrig -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_LINALG_PREVIEW=ON
cmake --build build/x64-testrig
```

The fetch script downloads the pinned D3D12 Agility SDK 1.721.3 preview and DXC v1.10.2605.24 and verifies their hashes. Release and Store builds leave the preview out and always use shader cores.

To retrain the networks, run `python tools/train_neural_weights.py`, which rewrites `assets/neural/neural_networks.bin`.

Launch options: `--neural on`, `--neural-shading`, `--neural-textures`, `--neural-radiance-cache` and `--neural-tensor-cores` (each `on` or `off`), and `--neural-solo none|shading|textures|cache`. The matching keys belong in the `[hardware]` section of `config/default.toml`.

### GPU and CPU sensors

Two lines beneath the frame rates read the hardware itself:

- **GPU** shows temperature, power draw against the card's power limit, the graphics clock and the load. `PWR` appears while the driver is holding the clock down for power, and `HOT` while it is doing so for temperature. The readings come from NVIDIA's management library (`nvml.dll`), which every GeForce driver installs. On a PC with several GPUs the app follows the one it is rendering on, matched by adapter identity, so two identical cards cannot be confused. On other vendors' GPUs the line reads `N/A`.
- **CPU** shows the load on the whole machine and this app's own share of it. Windows gives ordinary apps CPU load only; CPU temperature and power need a kernel driver, which the app does not install.

`--export-diagnostics` saves the last readings and the run's peaks and averages under `runtime.gpu_sensors` and `runtime.cpu`, including how often the GPU was power or thermally limited. The averages start when playback begins, so loading time is left out.

### GPU workout: the bird flock

The **Workout** row is a load dial. Level 0 is the film as authored. Each step up asks for more birds, and every one is a fully articulated bird, ray-traced where ray tracing is running. Press **[+]** and **[-]**, or `Shift+Up` and `Shift+Down`, and watch the frame rate, load, watts and temperature answer. The film's own birds never change: they keep their places and their flight.

| Level | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 (`MAX`) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Birds asked for | 40 | 160 | 400 | 1,000 | 2,000 | 4,000 | 8,000 | 16,000 | 32,000 | as many as the machine can fly, up to 200,000 |

**The frame-rate floor.** The dial asks; the machine decides. Birds are released in batches. After each batch the app lets the frame rate settle, measures it, works out what a bird costs on this machine, and only then releases the next batch. It never takes the frame rate under the floor, which is film speed, 24 frames per second. To stay ahead of the floor it aims for 28 and holds there. If the frame rate falls anyway, because the film cuts to a heavier shot or another program takes the GPU, it lets birds go at once. So level 10 is not a fixed number: it is the largest flock this machine flies at about 28 frames per second, and that number is a score for the machine. While birds are still arriving the row reads `RELEASING`; when the frame rate, and not the dial, is setting the size of the flock it reads `FLOOR 24` and the count turns red. Newly released birds grow in over a moment instead of appearing whole.

On an RTX 5080 Laptop GPU in a 1600×900 window with DLSS Quality, level 10 in the kettle formation settled at about 105,000 black kites at 28 frames per second, and the climb from none took about 18 seconds. A slower GPU settles lower; the levels below it simply stop where the floor is.

Memory grows with the flock. Up to 4,000 birds the app uses no more than it did before. Past that it makes room as the flock grows, about 8 KB of system memory for each bird of room, in steps of up to double. A flock of 100,000 therefore takes between 0.8 and 1.6 GB, plus GPU memory for the ray-tracing structure. If the machine cannot give more, the flock stops where it is.

The **Flock** row sets how the extra birds fly. Click the name, or press `Shift+F`, to change it. The formations are the types ornithologists describe, flown by a species that flies them in life, and a larger flock grows the way real ones do: more skeins, a longer cluster, a taller kettle, and then more kettles. The row shows the species by its scientific name.

| Formation | Species | What you see |
| --- | --- | --- |
| Cluster | The film's birds: black kite (*Milvus migrans*) in Taipei, great egret (*Ardea alba*) elsewhere | An extended cluster: a loose flock, longer than it is wide, following the film's birds round their circuit. |
| Vee | Great egret | A line formation. Skeins of 5 to 21 birds fly in a vee with the leader at the apex. The arms are rarely equal, and a very lopsided vee is a J. |
| Echelon | Great egret | A line formation: one staggered diagonal behind the leader. |
| Column | Great egret | A line formation: line astern, each bird a little above the wake of the one ahead. |
| Front | Great egret | A line formation: line abreast. |
| Kettle | Black kite | Kites circle up a thermal with their wings held out, all turning the same way. The column widens with height and leans downwind. At the top each bird glides out and down, then rejoins at the base. |

In the line formations each bird holds station about one wingspan to the side of the bird ahead, and beats its wings to the leader's rhythm delayed by its distance behind, so each wingtip follows the path of the one in front. Skeins keep their own lanes and heights. Black kites do not fly in lines and great egrets do not kettle, so choosing a formation also chooses its species. Speeds match the film's own unhurried birds, about a third of life speed.

Past 4,000 birds a flock is more of the same, and the birds already flying keep their places. A kettle holds 4,000 kites; the next 4,000 fill a second thermal farther out and higher above the roofs, and so on, the way a big movement of raptors fills one thermal after another. Later skeins take wider and higher lanes, so the sky fills outward. Later birds of the extended cluster wheel wider and higher round the outside of the circuit, as a great flock does over a roost.

The names follow F. H. Heppner, "Avian flight formations", *Bird-Banding* 45 (1974), which sorts organised flocks into line formations (column, front, echelon, J, vee) and cluster formations. The wingbeat timing follows Portugal et al., "Upwash exploitation and downwash avoidance by flap phasing in ibis formation flight", *Nature* 505 (2014).

**CAM** on the Flock row, or `Shift+B`, switches on the bird camera. It leaves the film's shots and follows the flock instead, framing all of it and turning slowly round it, while the film and music carry on. For a line formation it follows the first skein, and for kettles the first kettle. Egrets are watched from above against the city, and kites from nearer their own height against the sky. It works at level 0 too, where it follows the film's own birds. Explore mode takes the camera back while it is active, and shots with no birds return to the film's camera.

Launch options: `--gpu-workout 0` to `10`, `--workout-floor-fps` (24 by default; any rate from 10 to 240, and the app aims a sixth above it; `0` switches the floor off, so every level releases exactly what it asks for), `--flock cluster|vee|echelon|column|front|kettle` and `--bird-cam`. The matching keys under `[hardware]` are `gpu_workout`, `workout_floor_fps`, `flock_formation` and `bird_cam`.

`--export-diagnostics` saves the result under `runtime.gpu_workout`: the birds asked for and released, whether the floor was holding the flock back, the flock last measured holding steady and its frame rate (`steady_birds`, `steady_fps`), and what one more bird costs on this machine (`microseconds_per_bird`).

### PhysX suite

The **PhysX** row, under Flock, dials through physics from NVIDIA's PhysX SDK, one genre at a time. Click it, or press `Shift+P`. It is in test-rig builds that include the SDK (`tools/fetch_physx_sdk.py`, then configure with `-DENABLE_PHYSX=ON`); other builds show `N/A`.

| Genre | What you see |
| --- | --- |
| Off | The film as authored. |
| Smoke | NVIDIA Flow, the PhysX family's smoke solver, simulating warm vapour live on the GPU. It rises from the foot of each thermal the kettling kites circle in, and leans downwind with them. The film's renderer marches the smoke against the scene, so towers and birds stand in front of it or behind it as they should. |
| Rigid | A shower of 3,000 enamel marbles on PhysX's GPU rigid-body solver. They fall over a few seconds onto one block of the real city (every building is a solid box in the simulation), scatter, and come to rest; half a minute later they rise and fall again. The row shows the step time and the bodies. |
| Fluid | A small cloudburst on PhysX's GPU position-based fluid solver: 8,000 water particles fall in the terrace wind over one block, pour off the buildings and run down into the streets, then rise again as fresh rain. Each particle is drawn as a glass bead. |
| Soft | Two dozen beneficial microbes, soft celadon blobs, on PhysX's GPU finite-element solver for soft bodies (each a mesh of tetrahedra): they fall onto one block, squash against the buildings and the street, wobble and settle; half a minute later they rise and fall again. Each is drawn as the ellipsoid that best fits its simulated shape, so the squash you see is the solver's. The row shows the step time and the microbes. |

PhysX is an accent, not the subject: each genre takes one block of the city and has its moment, and the film carries on around it. The heavy loads belong to the Workout dial. The PhysX runtime (version 5.13, with its CUDA GPU solvers) starts in the background the first time a PhysX genre is dialled in, on the same GPU and CUDA context as the rest of the app; started with `--physx rigid`, `fluid` or `soft`, it loads during the loading screen and its libraries count on the loading bar.

The row shows the simulation's GPU time (`SIM`), the ray march's GPU time (`RAY`) and the voxels of smoke in thousands. On an RTX 5080 Laptop GPU one plume takes about 0.25 ms to simulate and 1.3 ms to draw at 1080p. Flow runs on Vulkan beside the film's Direct3D 12, on the same GPU, and its device is released as soon as the genre is switched off. `--physx off|smoke|rigid|fluid|soft`, or `physx` under `[hardware]`, sets the starting genre; `--export-diagnostics` reports the figures under `runtime.physx`. More genres join as they are built; see `docs/physx_suite_plan.md`.

**Explore marbles on PhysX.** In builds with the PhysX SDK, the marbles Explore drops (`Y`, `F10`, or `M` inside Explore) roll on PhysX: the first drop starts the PhysX runtime, and a fifth of a second later the marbles carry on under PhysX from wherever they are, with continuous collision detection so a fast one never passes through the counter. They roll on the same countertop, basin, fixtures and walls as before. Other builds keep the built-in marble solver.

**The opening, "Last Light".** The full film now opens at dusk on the Taipei terrace. A low camera skims the reflecting pool towards the setting sun, so the water is a sheet of gold (at that grazing angle water reflects nearly half the sky; seen from above it reflects only a few per cent, so the shot is low on purpose). A vermilion silk banner streams in the terrace wind, then the camera turns upwind to a kettle of 32 black kites circling beside Taipei 101 as the sun sets and the city switches on, window by window, before it descends to the orchid as it blooms. The kites lean with the same wind that moves the banner. Thirty-two is deliberate: Taiwan's 2025 survey counted about a thousand black kites in the whole country. The blimp joins the city after the opening.

**Physics in the film itself.** Some of the film's own motion is now simulated with PhysX offline and played back from files in `assets/physics`, so it is identical every run and seeks exactly, and needs no physics library to play:
- The parcel case hangs from the courier drone on a real line and swings with the drone's motion and the wind as it is lowered to the dock (2:43 to 2:53.5), settling exactly on the dock. The honest swing is small, about 4 cm.
- Madame N's brass iris opens as a geared mechanism: its four vanes turn on their own pivots, coupled to an actuator ring by gear joints, following the stage director's iris value.
- Taipei 101's 660-tonne tuned mass damper swings in the observatory on its cables, against the tower's sway in the breeze, damped by its dashpots: up to about 16 cm.
- The "Last Light" banner is light silk, 1 m by 6 m, pinned along its top to a bronze yard and streamed by the terrace wind on PhysX's GPU cloth solver: it swings and streams by metres (up to about 5 m from hanging). When the PhysX dial is off, the dashboard's PhysX row reads `BAKED CLOTH` with the cloth's widest offset this frame and its node count, so you can see it is real simulation played back, never live.
- The four woven canopy ribbons of the enamel cathedral are cloth, simulated on PhysX's GPU cloth solver: each is a strip of fabric a little wider than the gap between its two bronze tendons, pinned along them, so the terrace wind fills it like a sail, about 10 cm deep, and it trembles in the gusts. The tendons stay put.

Reduced Motion scales them down; the banner plays a separately baked calm track instead, since scaling a sheet that has swung through tens of degrees would shrink it.

**Bird-route test.** `--physx-route-test` sweeps every bird the film can show (the film's own birds over two circuits, and the workout flock in every formation at three kettles' size) through both cities' buildings with PhysX queries, and logs any bird that comes within 5 m (about 16 feet) of a building. PhysX never steers a bird: fixes go into the flight data. Workout skeins beyond the first fly in staggered altitude bands above each city's roofline. Over Taipei two of the film's own kites, on the left of the flock, used to pass within 1.7 m of a building west of the terrace as the flock settles over the rooftops; they now fly 1.5 m and 3.75 m further in, the smallest moves the test found plus a quarter metre to spare, so every bird in both cities keeps its 5 m. When a film bird strays, the test reports the smallest move that would clear it.

### Test rig

`--test-rig`, or `test_rig = true` under `[hardware]`, switches on every GPU feature at once. That covers DXR, CUDA and Tensor workloads, High effects detail, High path tracing, DLSS Quality, Ray Reconstruction, the highest Frame Generation multiplier the GPU and driver support, Reflex Boost, and all three neural networks. Combine it with `--export-diagnostics` to save a machine-readable report of the run.

BOO SAH SAH enforces strict separation between hardware capability detection and verified live execution:

Click a row in the **Hardware** table to compare that engine at the current playhead. The five tabs below the split-view labels let you change engines while watching the same shot. Drag the divider to inspect an edge or reflection. The × button closes the comparison and returns to the previous HUD. These controls also work while paused.

The NPU row shows where the stage director runs. `LIVE` means the NPU has completed an inference. `CPU` means OpenVINO has run the model on the CPU. `SCRIPT` means a deterministic script supplies the controls. `WARMUP` appears while the director prepares or waits for its first inference, `OFF` when inference is disabled, and `FLT` for a fault. The Full HUD shows completed inferences, the last inference time in milliseconds, and completions in the past second in hertz. The GPU renders both halves. The left half uses the current shot’s direction and the right half uses its scripted lighting. Shenzhen holds its authored lighting, so both halves match there.

Press `4`, click the **[4]** shortcut beside the NPU row, or click the bottom NPU button for the guided sanctuary comparison. Close it with × or `4` to restore your previous playhead and play/pause state. Cue sheets without a sanctuary loop open the director comparison in place.

The codec row shows `IDLE` with empty measurements between uses. The film requests it during Signal Hijack, and you can also request it with the Codec A/B view or the `V` control.

From the full film or the standalone sanctuary, press `4` to compare directors. Drag the divider to see how the lighting changes across the same camera view at the same instant. The GPU draws both sides. The left names the backend supplying the controls and the right follows the scripted cues. Safe quality uses scripted cues during normal playback. The dedicated comparison can request NPU inference at Safe quality while respecting the NPU setting selected with `U` or `--disable-npu`.

- **Emerald `LIVE`** marks verified hardware work such as a DXR hit, CUDA particle integration, Tensor Core matrix operations, or an H.264 encode/decode round trip. The NPU runs asynchronously and updates its measurements when an inference completes.
- **Cyan `SUB`** marks a substitute for a GPU workload, such as raster rendering, compute-shader particles, a scalar field, or the shader-based codec effect. The director row names its CPU and scripted routes separately.
- **Amber `DEG` (`Degraded`)**: A hardware path timed out or missed verification (e.g., `--inject-npu-timeout`) and failed over smoothly without stalling the film.
- **Slate `OFF`** marks an unavailable or disabled workload.

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
  - Whenever a webcam is active, a high-contrast crimson **`LIVE CAMERA`** pill is rendered at top-center of the window—even when the HUD overlay is turned off (`Tab` Mode 3). Press **`[W]`** (outside Explore; in Explore press `F` first) to stop and release the camera hardware immediately.
  - No camera picture is ever written to disk or transmitted over a network, and all user file paths in logs/diagnostics are redacted (`<USER_HOME>`).
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

# A quick look at chosen moments of the full film (family edition only, about a minute)
./build/x64-release/boo_integration_tests.exe --film-captures --film-capture-seconds 4,18,30,318 --no-activate --capture-dir diagnostics/look
```

The sky is a scattering table baked into `assets/shaders/m2_mesh.hlsl` by `tools/gen_sky_table.py`. After changing the atmosphere or exposure in that tool, run it (add `--preview <folder>` for panoramas of every sun elevation), then `python tools/refresh_asset_hashes.py`.

Open `manifest.json` beside the captures for per-frame results. Add `--require-debug-layer` to either command for strict D3D12 warning validation; this fails if Windows Graphics Tools is missing. Without that component, a successful run records warning validation as unavailable. All automated application and integration-test launches must include `--no-activate`.

If graphics fail during playback, the app makes one recovery attempt while keeping its place in the film. The diagnostic option `--inject-device-lost` simulates a fault after GPU work has drained. It tests teardown and recreation, not a physical GPU reset or real driver TDR. For current measurements, hardware details, and limitations, read the [Film Quality & Robustness Report](quality_upgrade_report.md).
