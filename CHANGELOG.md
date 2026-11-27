# Changelog — BOO SAH SAH: The Glass Orchid Protocol

All notable milestones implemented in accordance with `docs/SPEC_v4.md`.

## 1.1.13.0 — Never frozen, and the city on every GPU

- Fixes the Store's two rejections. Version 1.1.10 appeared frozen at launch on an Intel UHD 620 laptop (10.1.2.10): it compiled two 1 MB dashboard shaders on the window's thread. On a cold start here its window did not answer for about five minutes.
- **The city now draws on GPUs without double-precision math** (Intel Iris Xe, Intel Arc cards, Snapdragon laptops by published reports). `m2_mesh.hlsl` chose between plain numbers (`c ? 0.5 : 1.0`), which DXC types as double; those GPUs cannot build such a pipeline, so every version up to 1.1.12 dropped to the emergency test picture there. 106 literals are now typed float; the film's frames are unchanged pixel for pixel. A unit test (`ShadersRunOnEveryGpu`) fails if any shader needs double or 64-bit integer math.
- All graphics start-up runs on a loader thread; the window's thread only paints and answers. The window opens before the hardware probe.
- The loading screen shows each step as it runs, with its file and time, and says on a first start why it is the longest (25 languages).
- Integrated graphics start light: the Safe preset, raster lighting, no ray tracing resources and no neural rendering suite. Cold first start on Intel Iris Xe: 6 s.
- The dashboard shader's first text pass stands outside its loop: about 190 ms to 5 ms a frame on Intel integrated graphics, pixel-identical over 3,320 dashboard states; NVIDIA unchanged.
- If the city still cannot be built, the built-in scene draws instead. If the emergency test picture appears, a message names the GPU and the reason and offers to open the report page.
- Closing during loading hides the window at once. A frame that waits too long for the display is paced on its fence instead of being dropped. `--low-power-gpu` runs on the integrated adapter for testing.
- Optional graphics abilities are checked before use. An adapter without Shader Model 6.0, or a graphics start that fails, ends with the unsupported-graphics message. The neural suite needs compute reads of 16-bit float and 8-bit images, and the ray-traced city needs wave operations; where the suite is not built, its controls stay off and the log says why.
- The unit tests find the built shaders when CTest runs them. The privacy policy names the files the app keeps; the app's own captures never include the camera.
- Closing during loading starts nothing more (no sound, camera or benchmark). A benchmark after a light start runs without ray tracing. NVIDIA's runtime starts only on NVIDIA graphics, on the loading thread. A ray-traced pipeline the driver cannot build now costs ray tracing, not the city. If no picture at all can be built, a message says so.
- Graphics recovery runs on its own thread, so the window keeps answering. The camera runs only while its LIVE CAMERA label can be shown. The listing says ray tracing needs a supported card. The camera also stops when the window is minimised or too small for its label; closing or failing during a recovery ends cleanly.
- AMD graphics start with raster lighting: on a Radeon Pro V710 the ray-traced city hung the card and the app exited. After any graphics reset the app now recovers without ray tracing.

## 1.1.12.0 — No wait on the first start

- The first start after an update no longer holds on the loading bar, and reaches the film in 27 s instead of 58 s on an RTX 5080 Laptop GPU (`--cold-shader-cache`). The two shaders that draw the HUD over the film took the driver 44 s each to compile (53 s alongside the rest): every text call (`sampleWord4/8`, the number and timecode helpers, the stroke font) was inlined at its call site, about 1,500 copies of the glyph code in 1.1 MB of DXIL. Now each text call is a *run*, and a pixel draws the HUD in passes: each pass draws with the coverage of the runs resolved so far and notes the first unresolved run whose box holds the pixel, and a single glyph evaluator rasterises it between passes. 97% of pixels need one pass, none needed more than six over 3,600 HUD states. The shaders are 191 KB and 199 KB (with frame-generation outputs) and compile cold in 1.5 s; DXC builds them in 2 s instead of 37.
- Frames without the overlay or in hudMode 3 use their own pipelines (`-DBOO_PRESENT_CLEAN`, 45 KB, 0.16 s to compile) that leave out the overlay-only layers, so their registers stay few.
- The neural rendering suite's eight pipelines compile side by side (2.3 s instead of 8 s one after another, with the bar frozen at 78%). The longest wait left on a cold start is the two ray-traced city pipelines, about 15 s, while the bar creeps on the memory gauge.
- The HUD takes about half the GPU time. The present pass at 3840x2160: full dashboard 1.10 to 0.56 ms, A/B 0.66 to 0.41 ms, compact 0.56 to 0.51 ms, studio dock 0.67 to 0.59 ms, overlay hidden 0.18 to 0.15 ms (with frame-generation outputs alike). In the app at the default settings, GPU frame time p50 10.0 to 9.6 ms.
- The HUD looks the same. Of the 102 stable golden captures, 92 are byte for byte identical; the 10 that show the HUD differ in 62 pixels, by one level of 255, on the edges of glyphs (the stroke maths is no longer constant-folded per call). Comparing old and new shaders directly over 3,600 HUD states (every hudMode, panel, A/B engine, codec view and inset, captions, end card, LIVE CAMERA, the full-screen pill, translated labels, frame-generation outputs) gives at most one 10-bit level on about 3 pixels in a million; the hudless scene output is identical.
- With far less shader work left to the driver, the process now leaves Task Manager about 6 s after closing a first start instead of 43 s.
- The app looks and sounds closed the moment you close it. After a start that compiled shaders (the first after an update), releasing the D3D12 device waits while the graphics driver finishes its own background shader work: 43 s after a cold start, under 0.1 s once the cache is warm. Until now the music played on and, when closed with Esc, the window sat frozen on screen through that wait. The sound now stops and the window leaves the screen and taskbar first; the driver then finishes out of sight, so its cache is still complete for the next start. `--log-level debug` logs each shutdown step's time.
- The film ends gently. The music fades out over its last 8 seconds and is silent as the picture ends, instead of stopping dead on the last frame (`AudioEngine::setEndFade`, set each frame from the time left; not when the film loops or in the free camera). Once it has ended, the transport bar's PLAY and NEXT become one PLAY AGAIN button, and the studio dock's NEXT SCN reads PLAY AGAIN; either restarts the film. The label is translated in all 25 languages (catalog key `hud.play_again`, the atlas's last free cell).

## 1.1.11.0 — A faster first start

- The first start after an update takes 58 s instead of 2½ minutes on an RTX 5080 Laptop GPU. After every app or driver update the graphics driver's shader cache is empty, and timing each stage showed where the time went: 44 s for each of the two shaders that draw the dashboard over the film (plain, and with frame-generation inputs), about 16 s for each of the two ray-traced city shaders, and under half a second for everything else. The CUDA, tensor and codec runtimes load in under 0.1 s. Those big pipelines now compile side by side on threads of their own while the rest of start-up carries on (`PipelineCompiler`).
- The loading bar has a fourth gauge, shader code compiled into pipelines (`Shaders 2.2 / 4.3 MB` under the bar), against what the last start compiled. While start-up waits for those compiles it keeps repainting the bar and answering the window, which can be moved or closed.
- The bar still holds for about 20 s near the end of a first start after an update, while the dashboard shaders finish. Later starts are unchanged: 7 s to the film, pipelines from the cache.
- `--cold-shader-cache` starts as after an update: the driver ignores its shader caches (needs Windows Developer Mode). `--log-level debug` logs how long each start-up stage and each of those pipelines took.

## 1.1.10.0 — Street life

- Taipei's mapped streets are lined with trees and lamps: about 1,200 street trees (broad crowns and taller narrow ones, every 10 m on the pavement) and 650 lamps whose luminaires glow at night. They are placed by walking every kerb of the surveyed roads, and an occupancy grid keeps them clear of junctions, walls, water and elevated roads.
- Groves of five to seven trees fill the open ground between the blocks round the terrace and Taipei 101: parks, scattered clumps and clearings, set by a slow noise field and thinning out at the edge.
- Planters line the Xinyi arcades below the terrace.
- The generated towers' roofs vary: a crown on some, and on the rest a plant deck (tank, condensers, a mast), a roof garden or a lit blade sign.
- Lit shopfronts and lobbies glow round the foot of every generated tower at night.
- Over the open city at night the moon stands lower, so the towers' faces take a raking light. The terrace keeps its high key.
- A tree or lamp is one instance: the shader tells bark from leaves and pole from lit head by height, in raster and in ray-traced reflections alike. Trees, lamps, groves and rooftop sets are not collision geometry, so bird routes and PhysX bodies are unchanged.
- Cost on an RTX 5080 Laptop GPU at 1440p (city baseline, 60-frame medians): 0.2 to 1.4 ms on Taipei shots, highest median 13.9 ms.

## 1.1.9.0 — A new sky

- The sky is computed from how sunlight scatters in the air: Rayleigh, Mie and ozone single scattering at 16 wavelengths plus multiple scattering, baked for ten sun elevations by `tools/gen_sky_table.py` into the city shader. The same sky lights everything outdoors, colours the haze and lights the clouds, whose sunward edges stay lit after the sunset below and redden as the light goes.
- Nights are no longer black. The sky holds the blue hour (the sun 6.5 degrees below the horizon where it set), with an afterglow on that bearing, so the skyline and its lit windows read clearly. The low air at night is dark and blue, and the sky's fill is eased, so far windows still show through the haze.
- "Last Light" keeps a truthful golden hour: a warm white balance while the sun is low, and a warmer glow round the sun, which the reflecting pool mirrors.
- Lit windows and signs glow softly (bloom from brightness 0.8 instead of 1.35, spread wider), and the final grade adds a gentle print saturation.
- `boo_integration_tests --film-captures --film-capture-seconds 4,18,30` captures chosen moments of the full film for a quick look.

## 1.1.8.0 — "Last Light"

- A new 45-second opening at dusk: the sunset in the reflecting pool, a silk banner streaming in the terrace wind (baked PhysX GPU cloth), a kettle of 32 black kites leaning with the same wind beside Taipei 101, the sun setting and the city's lights coming on, then the orchid. Written by Claude Fable, reviewed by Codex and Antigravity.
- The dashboard's PhysX row reads `BAKED CLOTH` while baked PhysX cloth is on screen with the dial off.
- The renderer's moving-mesh path now drives any number of baked cloth surfaces (the canopy refits its ray-tracing structures, the banner rebuilds its own each frame).

## 1.1.7.0 — Hardware sensors, the GPU workout, the bird flock and the PhysX suite

- The dashboard reads the GPU's own sensors: temperature, power against its limit, clock and load, with a warning while the card is power or thermally limited. On a PC with several GPUs it follows the one the app renders on. A CPU line shows system load and the app's share.
- A **Workout** dial adds ray-traced birds to load the GPU: 40 at level 1, 32,000 at level 9, and at level 10 as many as the machine can fly, up to 200,000. The film's own birds are unchanged.
- The workout never takes the frame rate under a floor of 24 frames per second. It releases birds in batches, checks the frame rate after each, aims for 28 to stay ahead of the floor, and lets birds go at once if the frame rate falls. `--workout-floor-fps` changes the floor or switches it off. The flock is posed on every CPU core, so the GPU stays the limit.
- Past 4,000 birds a flock grows as real ones do: more skeins in wider lanes, kettle after kettle in further thermals, a broader extended cluster.
- A **Full screen** button in the top-right corner. Full screen it reads **Exit full screen**, and `Esc` now leaves full screen instead of closing the app.
- A **PhysX** row (test-rig builds with the PhysX SDK): `Shift+P` dials through NVIDIA PhysX genres: smoke (NVIDIA Flow vapour rising through the kites' thermals), rigid (a shower of enamel marbles on the GPU rigid-body solver, landing on the real buildings), fluid (a small cloudburst on the GPU position-based fluid solver) and soft (two dozen soft microbes on the GPU finite-element soft-body solver, squashing onto the buildings). Each is an accent on one block of the city.
- Film physics baked offline with PhysX: the parcel case swings on its line, Madame N's iris opens as a geared mechanism, Taipei 101's tuned mass damper swings in the tower, and the canopy ribbons are GPU cloth billowing in the terrace wind.
- The renderer can now draw meshes whose vertices move (the cloth first): raster, ray tracing and the path tracer see the same surface, and DLSS gets per-vertex motion vectors.
- `--physx-route-test` checks every bird route against the buildings with a 5 m clearance; the workout flock's flight data was adjusted to pass, and two of the film's Taipei kites now fly a little further in from a building they passed within 1.7 m of.
- The Explore marbles roll on PhysX in test-rig builds, with continuous collision detection; the built-in solver remains for other builds.
- A real loading bar at start-up. It counts bytes: files read into memory, system memory and GPU memory as they fill, against what the last start on this machine reached. Every asset the manifest lists is checked against its SHA-256 as it loads.
- The extra birds fly real flight formations, named as ornithologists name them: an extended cluster behind the film's birds, great egrets (*Ardea alba*) in vee, echelon, column and front, or black kites (*Milvus migrans*) in a thermal-soaring kettle.
- A bird camera follows the flock in place of the film's shots.
- `--export-diagnostics` reports the sensors, CPU load and workout.

## 1.1.6.0 — Neural rendering, Tensor Cores and a collapsible dashboard

- A **Neural rendering** section in the upper-left dashboard runs three neural networks on the GPU every frame: neural shading, neural texture compression and a neural radiance cache that trains online. Each row shows its GPU time and quality in dB. Click a row to switch that network, click its badge to solo it, and press `Shift+N` to switch the whole suite. The suite is off by default.
- `--test-rig`, or `test_rig = true`, switches on every GPU feature at once. `--export-diagnostics` now reports the neural figures.
- Test-rig builds (`ENABLE_LINALG_PREVIEW`) run network inference on Tensor Cores through the Shader Model 6.10 preview. The dashboard shows `TC` or `SC`, and Shift-clicking the heading switches between them.
- The upper-left dashboard collapses. Press `` ` `` or use **[-]**, **[+]** and **[X]** to switch between the full table, a two-line mini strip, and a small **HUD +** tab.
- `tools/train_neural_weights.py` trains the offline networks into `assets/neural/neural_networks.bin`. The neural texture comes from the film's own Glass Orchid frame.

## 1.1.5.0 — Cinematic camera and real-time graphics (release candidate)

- Circle the Glass Orchid and Taipei 101 in two separately framed shots. A survey-based terrain mesh extends around the Taipei basin. The camera also follows the river patrol, a parcel delivery, the black kites and the aircraft over Shenzhen.
- Change path-traced lighting, DLSS upscaling, Ray Reconstruction, Frame Generation and Reflex from the upper-left dashboard. Full and Compact views show the selected mode and measured state in all 25 supported languages.
- View rendered and presented frame rates alongside the hardware workloads. The NPU displays its inference time and rate independently.

## 1.1.3.0 — The seven-scene film and hardware comparisons

- Taipei gains covered arcades, ceramic walk-ups and stepped office towers. Shenzhen’s waterfront surrounds the aircraft with more varied rooflines and lit floors. Repeated building parts share geometry in the normal view and in ray-traced reflections.
- Nearby windows show six authored room interiors as the camera moves. Distant blocks use grouped window lighting that stays readable across the skyline.
- The Hall of Mirrors has etched brass panels, and gold and jade threads run through the Neural Velvet canopy. The director steers two flower-shaped pools of light across Madame N’s stage. Particle Mutiny has two colored streams with tracers that follow their motion. Pearly enamel grows from the bases of the finale’s crystal petals before the dawn reveal.
- Click a hardware row to compare its effect during any act, including while paused. Switch engines using the tabs above the image, then close the comparison to return to the previous HUD. The NPU comparison shows the active director and scripted lighting on the same GPU-rendered shot. The **[4]** control still opens the sanctuary demonstration.
- The soundtrack now starts with the film clock after graphics warm-up. Slow startup keeps the opening music aligned with the first scene.

## [Unreleased] — Soundtrack and director comparison

- The bundled soundtrack uses lossless recordings from the four WAV masters. Playback applies the volume control directly. Music imported through the file picker or drag-and-drop keeps its original filename, including Unicode characters.
- Press **4** to compare the neural stage director with scripted lighting in the same sanctuary shot. The Hardware table identifies the active route and reports completed inferences, latency, and measured rate. Seeking uses scripted lighting until the model returns a result for the new scene.
- The codec row reads **Idle** between Signal Hijack scenes.

## 1.1.0.0 — Shenzhen aircraft and recursive effects

- The opening camera looks out across Taipei 101, arcaded shophouses and nine rows of city blocks extending toward the mountains. The Shenzhen waterfront gains an elevated railway, a five-car train, a twin-leg tower with skybridges, and outer neighborhoods around the bay.
- The camera follows the six-rotor aircraft through its approach over Shenzhen. The cockpit, wings, and tilt rotors have new geometry, and six iridescent wakes follow the aircraft down to the pad.
- Cinema mode keeps the selected rendering resolution during the intercept sequence. Telemetry carries the compressed broadcast in an inset. The full-screen codec and A/B controls remain available.
- Press **7** to change effects detail. Higher settings add finer recursive patterns to Neural Velvet, more particles, and detail behind city windows. Safe mode uses the lowest setting. The control is localized in all 25 display languages.

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
