# BOO SAH SAH — The Glass Orchid Protocol
## Technical Master Specification v4.0

**Status:** Implementation-ready design specification; no implementation or benchmark success is claimed.  
**Date:** 2026-11-10  
**Primary platform:** Windows 11 x64, native C++20 application.  
**Reference machine:** Lenovo Legion Pro 7i, Intel Core Ultra 9 275HX, NVIDIA RTX 5080 Laptop 16 GB VRAM, 64 GB RAM.  
**Format:** Six-minute, real-time, noninteractive audiovisual demoscene production.  
**Principle:** *The story always continues, even when one accelerator cannot.*

---

# 0. Executive decisions — locked

1. Official project name **BOO SAH SAH**; subtitle **The Glass Orchid Protocol**. Treat the name as a creative title, not a claimed translation of Taiwanese/Hokkien/Mandarin.
2. **Absolutely no gameplay:** no player avatar to control, goals, levels, challenges, combat, score, inventory, unlocks, streaks, or penalties. The audience watches a show. Playback, seeking, scene selection, quality settings, and diagnostics are allowed.
3. **Six-minute primary film (360 seconds)** with original music and three distinct reveals near the opening, midpoint, and end. Loopable final act for VJ use.
4. **Taiwanese-inspired spy-tech glamour**, Taipei-rooted art direction, sophisticated, funny, humane, nonviolent. Fictional institutions and adult characters with agency. Two edits: **Family** and **Midnight** (adult-oriented wit/romantic intrigue, but no explicit sexual content or violence).
5. **Dental microcosm imagery** remains integral: enamel, biofilms, beneficial microbes, floss, saliva, fluoride, mineral crystals, acid-clock abstractions. These are *cinematic metaphors* rather than dental gameplay or clinical advice.
6. Four primary computational acts: **DXR/RT**, **Tensor**, **Intel NPU**, and **CUDA**. A fifth **Combined** show fuses the four. **Hardware Solo Mode** is required and must offer each accelerator separately, plus Full Film and Finale Loop.
7. **NVENC is optional and non-blocking.** No recording, stream publishing, or user video is required. An optional **Signal Hijack** module can ingest user media and perform live in-memory NVENC→NVDEC recompression for visual art. Without it, the film proceeds with a shader-generated or preauthored signal-distortion scene *labeled as such*.
8. **Optional NPU at all times.** An unavailable NPU must never stop timeline playback. Explicitly try a compatible Intel NPU, then a compatible GPU/CPU neural inference engine, then a scripted director. Arm64/Qualcomm NPU is a separately tested future port, not an automatic drop-in replacement for x64 OpenVINO.
9. All accelerators are **honestly verified**. “Solo” indicates the *featured computation*, not exclusive execution by that core type. D3D12 shaders/compositor, CPU scheduling, and memory transfers remain necessary.
10. **Offline first**, no account, no telemetry by default, no camera/microphone permissions unless explicitly requested; no hidden downloads; no compulsory cloud. Local diagnostics and user-selected input only.
11. The feature roadmap is **small verified vertical slices**, not an entire engine implemented in one pass.
12. Target **1080p60**, with measured dynamic degradation to **900p/720p and/or 30 fps**. An achievable first-run experience is more important than an unverified target. Budget normal operating VRAM <= 12 GB, elevated soft ceiling <= 14 GB, with margin for the desktop, OBS, drivers, and other applications.

# 1. Product boundaries and supported environments

## 1.1 Windows development targets

| Tier | Example | Required behavior |
|---|---|---|
| A: Reference | RTX 5080 Laptop + Intel 275HX NPU | Full film, DXR, CUDA, Tensor, NPU where driver/model works; optional NVENC |
| B: NVIDIA + no NPU | RTX GPU with DXR/CUDA, conventional CPU | Same film; CPU or GPU neural director; clearly identify substituted NPU |
| C: DXR GPU, no CUDA | AMD/Intel DirectX 12 GPU | Ray-traced or raster path; HLSL compute particles; procedural material substitute; CPU director |
| D: Older raster GPU | DX12 without DXR | Rasterized mirrored scenes, reduced particles, procedural texture stand-ins |
| E: Weak/WARP test | No suitable hardware graphics | Informative compatibility screen and minimal low-fidelity playback *if tested*; cannot promise realtime film |
| Future Arm64 | Windows on Arm | Requires native build, compatible graphics backend, optional QNN integration and full test matrix |

**Minimum supported baseline for first production release:** A working Windows 11 D3D12 rendering device and the basic assets. Unusable D3D12 devices must fail with a readable diagnostic rather than an exception or black screen. A software-only playback fallback may be developed later, but do not represent it as working until tested.

Do not conflate an ARM CPU with an NPU. The optional QNN adapter is only activated when a supported Qualcomm NPU runtime, driver, model, and native binary are all present.

## 1.2 Explicit non-goals

- No dental diagnosis, personalized oral-health conclusions, or medical data collection.
- No dental arcade controls, habit scoring, or Brush-Along timers in the film. Those belong to a separate MouthLife interactive product, if ever built.
- No online multiplayer, quests, game physics as a player challenge, AI chatbot, speech recognition requirement, or generative-language requirement.
- No mandatory encoder, video recorder, YouTube upload, livestreaming, or cloud service.
- No arbitrary memory/bitstream corruption against drivers; no unstable graphics-driver exploitation.
- No claims that custom shaders run *inside* RT/Tensor/NVENC fixed-function blocks.

# 2. Creative bible: story, sound, visual language

## 2.1 Overall look

An atmospheric, polished **Taipei-at-night espionage fantasy**: rain reflections, neon typography in properly researched Traditional Chinese, polished glass laboratories, a stylized metro, dense urban architecture, old spy optical equipment, chrome teeth interpreted as impossible architectural objects. Not a generic “Asian cyberpunk city”; place-specific references, accurate signage, street geometry, and local research need review. Fictional espionage agency: **The Glass Orchid Institute**.

**Palette:** jade/teal, orchid magenta, amber-gold highlights, midnight charcoal, warm enamel-white, iridescent spectral color. Physically plausible glass and specular highlights in RT sections; intentionally synthetic plasma/mathematical color in CUDA/Tensor sections.

**Cast (preauthored animation, never mandatory AI avatars):** Mei-Lin Chen, inventive adult Taiwanese field director; Dr. Iris Lin, microbial ecologist; Madame N, witty and theatrical stage director. They are active participants in the story rather than decorative figures. Family edit has light comic banter. Midnight edit has more sophisticated noir, dry humor, stylish romance between consenting adult characters. Neither contains violence, coercion, or explicit content.

## 2.2 Timeline contract

All cue ranges use film-time seconds, left-inclusive/right-exclusive, with a final hold at 360.000 s. Each act must also have an independent loop-safe entry and exit for Hardware Solo Mode. Scene transitions should use pre-roll without changing the public total duration.

| Time | Act / featured engine | Cinematic event | Audio | Reveal |
|---|---|---|---|---|
| 00:00–00:45 | Taipei: Glass Orchid / RT+raster | Spy-film opening, chase **without violence**, camera plunges into chrome city | Orchestral synth-noir / distant city-pop | **Opening** at ~00:36: “city” is the enamel topology of a tooth |
| 00:45–01:40 | Mirror Intelligence / DXR | Glass orchid corridors, layered reflections, optical codes | Crystal percussion and rhythmic synth pulses | No |
| 01:40–02:30 | Neural Velvet / Tensor | Neural textures reveal microscopic living surface patterns | Lush harmonic electronica | Build uncertainty |
| 02:30–03:15 | Madame N / NPU | Director reclassifies apparently sinister “agents” as beneficial microbial residents | Suspense dissolves into jazz/lounge | **Midpoint** at ~02:55: protect balance, not eliminate all life |
| 03:15–04:05 | Particle Mutiny / CUDA | Toronto plaque towers, New Orleans gumline, Tokyo/Chicago acid-clock, Accra/Colorado mineral coast, orbital station in rapid montage | Jazz-funk → highlife influences → techno | No |
| 04:05–04:35 | Signal Hijack / optional NVENC | Spy transmission degrades; secret orchid appears through compressed or shader-generated interference | Broken-beat video-feedback textures | Technical surprise, not plot-critical |
| 04:35–06:00 | Silicon Rebellion / all available | Orbital crystal bloom, multiplying glass petals, worlds fold into a giant radiant orchid; return to ordinary toothbrush/mirror | Grand demoscene finale | **Final** at ~05:42: the whole epic is a poetic celebration of everyday oral care |

This timeline is a **design target**, not an already-produced film. Final precise music beats are stored in data, not scattered across engine source code.

## 2.3 Dental/science policy

- Enamel primarily consists of mineral hydroxyapatite; do not state it is categorically “harder than all steel.”
- pH ~5.5 is a useful approximate acid-demineralization teaching concept, not a universal switch applying to all mouths or materials.
- Fluoride supports enamel remineralization and resistance under appropriate conditions; visuals must not claim instant clinical repair from a magic surge.
- Beneficial microorganisms and ecological balance are central to the midpoint reveal; avoid language implying sterilization of the whole mouth is desirable.
- Saliva supports buffering; a fantastical spring is clearly allegorical.
- Interdental cleaning imagery should not be presented as a replacement for real oral care.
- Historical claims (early Chinese bristle toothbrushes, van Leeuwenhoek, Parmly, Bass, Stephan, fluoride research, spaceflight, Costerton) require source verification prior to release. Do **not** silently import unverified historical copy from the original game pitch.
- Film end card may say: “A celebration of oral care—not medical advice.” Keep this tasteful and brief.

## 2.4 Audio specification

- Entire soundtrack must be original, user-licensed, or explicitly public domain with verified provenance.
- **48,000 Hz stereo floating-point mixing**, with 24-bit optional output device path; design original compositions (not copy a specific commercial track).
- Audio callback cannot block on GPU/inference work. Lock-free/single-producer queues for cue delivery; preallocate DSP buffers.
- Use a **48 kHz sample-count master clock** for film timing when audio runs. The six-minute score spans 17,280,000 stereo sample frames.
- At startup, when no audio device is available, switch to monotonic high-resolution clock and continue silently; preserve cue timing.
- Precomputed beat grids, energy envelopes, section cues and harmonic metadata embedded as data for deterministic offline playback; live FFT is optional for external audio-reactive mode.
- Original procedural/looped arrangements: Taiwan-influenced indie synth/city pop, elegant lounge jazz, New Orleans jazz-funk, highlife-inspired rhythmic textures, demoscene tracker-style techno, and cosmic space lounge. Research/collaborate to avoid superficial cultural clichés.
- Frame presentation reads the audio time at start of frame. On pause, seek, audio restart, and device switch, rebase a stable timeline clock.
- Optional dialog is fully captioned, localized where possible, and never required to understand visuals. Keep spoken lines short and accessible.

# 3. Runtime architecture

## 3.1 Language, renderer, tools

- **C++20**, MSVC 64-bit, Visual Studio 2022 or later, **CMake 3.28+**, Windows SDK / current DXC HLSL compiler.
- **Win32** application window and D3D12 graphics device; **DXGI flip-model swap chain** (triple buffering), D3D12 graphics+compute queues where measured beneficial.
- DXR via **D3D12 raytracing API**, fallback D3D12 raster/PBR/SSR or planar reflection approximation.
- CUDA C++ runtime only as a **dynamically detected optional plugin**; use an SDK version supporting RTX 50 series/Blackwell. Never require CUDA DLLs to launch baseline raster film.
- Tensor module: **cuBLASLt** or direct CUDA tensor-math library with actual half-precision GEMM. Use kernels supporting device/driver; no claim of Tensor acceleration from merely calling a CUDA kernel.
- NPU module: **OpenVINO runtime** targeting explicit `NPU`, then tested CPU inference. A future ONNX Runtime + QNN provider is a **separate optional adapter** for Windows Arm64, not part of x64 MVP.
- Media ingest: Windows Media Foundation for supported files and webcam (only with user action). Optional codec library integration later; file extensions never guarantee actual decoding support.
- Optional encoder module: NVIDIA Video Codec SDK NVENC+NVDEC, separate DLL/plugin loaded on demand, in-memory processing only. Safe shader-simulated visual substitute when unsupported.
- Optional Spout2 **D3D12 sender** after main playback is stable; OBS/Resolume receiver interoperability requires actual testing on the installed versions.
- Dependencies locked by a reproducible manifest (vcpkg/CPM/CMake FetchContent as appropriate); note redistributable licenses, proprietary SDK prerequisites, optional runtime components.

## 3.2 Repository skeleton

```text
boo-sah-sah/
  CMakeLists.txt
  CMakePresets.json
  README.md
  LICENSES.md
  third_party/README.md
  docs/
    SPEC_v4.md
    architecture.md
    content_bible.md
    hardware_matrix.md
    profiling_protocol.md
    risk_register.md
  assets/
    cues/film_360s.json
    cues/solo_rt.json
    cues/solo_tensor.json
    cues/solo_npu.json
    cues/solo_cuda.json
    shaders/
    models/README.md
    audio/
    fonts/
    geometry/
    textures/
  config/
    default.toml
    quality_presets.json
  src/
    app/{main,window,settings,diagnostics}.cpp
    core/{clock,cues,event_bus,health,capabilities,logging}.cpp
    render/{d3d12_context,swapchain,framegraph,pbr,dxr,postfx}.cpp
    effects/{scene_rt,scene_tensor,scene_npu,scene_cuda,scene_signal,scene_finale}.cpp
    compute/{cuda_bridge,cublaslt_field,d3d12_compute_particles}.cpp
    inference/{director,openvino_adapter,scripted_director}.cpp
    audio/{engine,transport,features,synth}.cpp
    media/{source,media_foundation_input}.cpp
    integrations/{spout_sender,nvenc_feedback}.cpp
    ui/{transport,settings,diagnostics_overlay}.cpp
  include/boo/{api,capabilities,cues,frames,health}.hpp
  plugins/{cuda,tensor,npu,nvenc}/
  tools/{asset_validate,cue_validate,shader_compile,bench_report}/
  tests/{unit,integration,smoke,media,performance,fixtures}/
  scripts/{configure,build,run_smoke,collect_profile}.ps1
  .github/workflows/
```

**Note:** Header paths above are organizational suggestions; they are not existing files. Organize core features around **interfaces and capability negotiation**, not Intel/NVIDIA names inside scenes.

## 3.3 Process separation and threading

Recommended MVP single process with fault-aware optional modules; do not attempt true GPU-driver crash immunity. A later isolated helper process is allowed for experimental codec and model code if needed.

| Thread/queue | Responsibilities | Never do |
|---|---|---|
| UI/main | Win32 messages, setting changes, input transport | GPU waits or blocking model compilation |
| Render | Framegraph, graphics submission, presentation, graphics resource ownership | Blocking file IO, media decode, model compile |
| CUDA compute worker | Optional compute queue, CUDA launch/interop events | Unbounded copies/synchronization |
| Inference worker | Async NPU/CPU inference, cached results | Stall film timeline |
| Audio callback | DSP/mixing, sample clock | Allocation, locks, logging, inference |
| Asset/media worker | Async preload/decode and bounded caches | Hold render thread during IO |
| Diagnostics worker | Sample telemetry, write opt-in logs | Claim fixed-function-core activity from total GPU utilization |

Use Windows high-performance GPU preference plus explicit DXGI adapter selection. On hybrid laptops, check device identity and adapter LUID for D3D12 vs CUDA resources. Do not assume GPU 0 in CUDA is the D3D display adapter.

## 3.4 Core C++ contracts (proposed)

```cpp
enum class BackendStatus {
    Accelerated,     // featured hardware actually ran this frame/segment
    Substituted,     // another implementation executed successfully
    Degraded,        // reduced quality, partial effect, or scripted stand-in
    Unavailable,     // no viable equivalent; story-safe effect omitted
    Faulted           // implementation errored; quarantine until retry/restart
};

enum class EngineKind { RT, Tensor, NPU, CUDA, Codec };
enum class PlaybackMode {
    FullFilm, SoloRT, SoloTensor, SoloNPU, SoloCUDA, FinaleLoop
};

struct HardwareResult {
    EngineKind engine;
    BackendStatus status;
    std::string requestedBackend;
    std::string activeBackend;
    std::string diagnosticCode;
    std::string humanReadableReason;
    bool successfullyExecutedTestWork = false;
};

struct SceneCue {
    std::string sceneId;
    double startSeconds;
    double endSeconds;
    uint64_t deterministicSeed;
    std::string visualPreset;
    std::string audioSection;
};

struct FrameContext {
    uint64_t presentationFrame;
    double filmTimeSeconds;
    double dtSeconds;       // bounded/clamped after stall
    bool seeking;
    uint64_t randomSeed;
};

struct IEffect {
    virtual ~IEffect() = default;
    virtual HardwareResult probeAndInitialize() = 0;
    virtual void preload(double atFilmTime) = 0;
    virtual void resetTo(double filmTime) = 0;
    virtual void render(const FrameContext&) = 0;
    virtual void shutdown() noexcept = 0;
};
```

Requirements: instrument actual execution, not capability detection alone; permit runtime downgrade; detach failed effects; bounded non-throwing shutdown; no global hard dependency from scene definitions to an optional plugin.

## 3.5 Capability detection and fallback algorithm

```
startup:
  initialize non-GPU audio/cue/diagnostic core
  enumerate DXGI adapters / choose render adapter
  create D3D12 device or show graceful unsupported-device diagnostic
  check DXR tier; create RT backend only if supported
  dynamically probe CUDA runtime and check same physical adapter
  test known-good CUDA kernel
  test a small cuBLASLt GEMM; log verified backend
  enumerate OpenVINO devices; explicitly compile/test compatible NPU model
  otherwise test OpenVINO CPU or alternative compatible model runtime
  otherwise select deterministic scripted director
  set NVENC state = not_loaded (unless user enabled Signal Hijack)
  create scene substitutes as needed
  preload first scene and begin film; do not block on slow optional subsystems

for each scene:
  choose best currently healthy implementation
  log chosen engine and backend
  present film according to audio master clock
  process async results only when ready
  on effect exception, resource failure or watchdog timeout:
      disable failed instance, activate a tested substitute
      preserve audio time and camera/cue progression
      expose status in diagnostics
  retry failed device only at safe scene boundary or explicit user request
```

**Startup safeguard:** Optional model compilation may continue asynchronously after the film starts using a scripted director. An NPU becomes active only after explicit model completion + a successful inference; show actual active backend.

# 4. Hardware Solo Mode — required feature

## 4.1 User-visible controls

Six clearly labeled presets: **Full Film, RT Solo, Tensor Solo, NPU Solo, CUDA Solo, Finale Loop**. An optional **Signal Hijack** item appears only after the module is enabled (and can still play a simulated version on unsupported hardware, labeled accordingly). Controls: Play/Pause, Restart, Skip to Next Act, Seek, Loop, Mute, Quality, Family/Midnight, Show Hardware Overlay. No gamepad is required, but optional transport hotkeys/controllers are supported.

Solo modes are **time-limited audiovisual acts with loop points**, not five independent games. They may reuse the film's act assets/music and provide extended generative variations through seeded procedural animation.

## 4.2 Solo engine contracts

### 4.2.1 RT Solo — Mirror Intelligence

- Render a **ray-traced reflective glass orchid** and infinite-looking tooth-arch corridor.
- Use DXR `DispatchRays`, BLAS/TLAS, raygen/miss/closest-hit shaders, HDR lighting, reflection recursion control, progressive/bounded temporal accumulation.
- A practical default: 1 reflection bounce or 1 sample per pixel at 1080p with temporal accumulation and denoising; increase only if measured headroom.
- Reflections need not be physically infinite: use a finite recursion budget with aesthetic compositing to suggest infinity.
- Probe support via `D3D12_FEATURE_D3D12_OPTIONS5` raytracing tier, successful shader/AS build, and executed ray dispatch.
- Fallback: PBR raster reflections, local reflection probes, SSR/planar approximation; never label this DXR.
- Overlay: active DXR/raster path, render resolution, samples per pixel, rays dispatched (calculated workload), GPU frame time. A DXR dispatch alone does not provide a hardware RT-Core occupancy percentage.

### 4.2.2 Tensor Solo — Neural Velvet

- Implement a **small neural field / network-driven texture synthesizer**, output RGBA or feature planes consumed by D3D12 shader.
- Coordinate inputs: normalized (u,v), sample time, selected audio envelope, scene seed. A small MLP with 2–4 layers and compact hidden width is sufficient for an MVP; matrix layout must result in meaningful GPU GEMM batches (e.g., tiles/coordinate batches) rather than thousands of tiny single-pixel launches.
- FP16 activation and weights using cuBLASLt or a verified Tensor-capable kernel; verify native matrix instructions via representative Nsight profiling during development.
- Output at quarter/half resolution; upscale/warp/postprocess in HLSL. Reuse GPU memory via D3D12/CUDA sharing only after tested sync path; for initial milestone, bounded copy is allowed and measured.
- Avoid large image diffusion models, token generation, and model downloads. Hand-authored or locally generated weights with deterministic seed are acceptable.
- Fallback hierarchy: tested generic CUDA math without confirmed Tensor execution -> pure D3D12 procedural HLSL visual -> CPU-generated low-resolution texture. Status must say which is active.
- Overlay: GEMM size (M,N,K), inference/compute milliseconds, tensor verification state ("profiled" versus "not independently confirmed"), output resolution, VRAM allocation.

### 4.2.3 NPU Solo — Madame N

- Show an **autonomous stage-director performance**, not falsely suggest that the NPU itself renders pixels.
- Small local neural model takes **audio features and timeline metadata** (e.g., 16- or 32-value feature vector: section energy, onset, low/mid/high-band envelope, tension proxy, progress, current visual state). Outputs probabilities or categorical **Calm / Suspense / Bloom / Comic / Transition**, plus 2–4 bounded mood parameters. Keep feature contract frozen across backends.
- Inference rate recommended **2–10 Hz**, asynchronous. Hold latest good result; apply smoothing/hysteresis and minimum state duration to prevent flicker.
- Use OpenVINO explicit target `NPU` and validate compiled-model actual execution. Do not rely on unqualified `AUTO` to select NPU: documented default AUTO priority can exclude it.
- Fallback: CPU OpenVINO model; optional compatible GPU inference; last resort **scripted deterministic director**, clearly identified as non-neural fallback.
- Experimental cross-platform adapter: ONNX Runtime/QNN on supported Windows Arm64/Qualcomm devices; each model/operator/driver combination must pass unit+runtime tests before claiming support.
- If NPU model fails at 02:55, the midpoint reveal still occurs on cue using scripted direction. NPU output can change lighting/mood subtleties **within constraints**, never destroy the story or move major reveals.
- Overlay: model ID and checksum, selected execution device, execution latency, inference count, last-update age, fallback reason, confidence values if meaningful.

### 4.2.4 CUDA Solo — Particle Mutiny

- GPU resident procedural particle buffers, preferably structure of arrays (positions, velocity, color, lifetime, seed). Start 100k particles at 1080p, expand adaptively to 250k/500k/1M as measurable performance allows.
- Kernels: integration (fixed/timestep bounds), curl noise/vortex, force fields, emitter/kill/respawn, density/color packing. Use sensible launch occupancy and avoid excessive per-particle random-memory traffic.
- Render via D3D12 structured buffers or geometry-instanced/point sprites; use shared memory/fence interop or measured staging path.
- Target effects: foam dissolve, mint clouds, mineral precipitation, microbial city abstraction, concentric orbital spirals. No simulation claims of actual fluid dynamics unless the solver implements them.
- Fallback: D3D12 compute HLSL, then reduced GPU/CPU particle counts if necessary. **HLSL compute is not “CUDA”** even when running on NVIDIA hardware.
- Overlay: particle count, update milliseconds, render milliseconds, algorithm/precision, active CUDA/HLSL/CPU status.

### 4.2.5 Combined Finale

- RT environment + Tensor material textures + CUDA particles + NPU director cues.
- Render dependencies must be explicit, double/triple-buffered as appropriate; never permit two APIs to write the same resource concurrently.
- Optional codec effect may appear, but **finale is complete without NVENC**.
- Allocate frame budget per stage and gracefully reduce each independently. If every optional accelerator is missing, deliver a coherent raster/procedural “orchid reveal,” even at reduced visual fidelity.
- Do not confuse all-engine **simultaneous eligibility** with simultaneous hardware occupancy. Measure resource contention and decide whether to overlap compute or serialize tasks for performance.

# 5. Signal Hijack — OPTIONAL codec art module

## 5.1 What it is

An experimental compression-feedback visual effect that can use (a) generated Glass Orchid visuals, (b) a user-selected local video file, or (c) a user-enabled camera feed. No input is ever required. No video is saved as a required output. No microphone or camera auto-activation.

**NVENC is not a shader or arbitrary computation engine.** It encodes frames according to supported codec standards. Glitches derive from codec parameters, repeated compression, temporal feedback, post-decode effects, and compositing. We cannot promise arbitrary “noise generation inside NVENC.”

## 5.2 Two implementations

**A. Real codec path (if enabled and supported):** D3D12 final/scene frame -> compatible NVENC input surface (may require CUDA/D3D11 bridge/format conversion) -> H.264/HEVC/AV1 in-memory bitstream -> NVDEC decode -> reconstructed frame GPU surface -> CUDA/HLSL feedback + compositing -> D3D12 framegraph. Check both encode and decode capabilities, codec/profile, and real compatibility of the chosen surface API. NVIDIA documentation specifically supports CUDA/D3D11 paths for certain vidmem bitstream features; do not assume arbitrary D3D12 textures can be passed directly into every API.

**B. Art-direction substitute:** HLSL macroblocking, chroma subsampling approximation, temporal echo, block-motion distortion, scanline/noise matrices. Label as **SHADER SIGNAL**, not actual NVENC.

Optional artistic controls: quantization/quality, bitrate if dynamic reconfigure is supported, GOP size, constrained feedback amount, time offset, chroma phase, blending. Make parameter ranges bounded and stable; avoid malformed bitstream fuzzing and driver-crash experiments.

## 5.3 Live media policy

- Local file picker is deliberate user action. Run supported file decode via Media Foundation; offer clear errors for unsupported codecs rather than promising all MOV/MKV files.
- Camera opt-in via OS API only when chosen, with visible “LIVE CAMERA” indicator and a one-click stop.
- Imported content remains local; never upload, index, or automatically save.
- Dynamic resolution cap on codec loop: begin 640x360 or 960x540 at <=30 fps, composite into 1080p output. Budget encode/decode latency; drop feedback frames instead of stalling the film.
- No hard NVENC install requirement; lazy-load plugin. Film opens and plays with no NVIDIA GPU.
- NVENC Solo is **not one of the required four hardware solos**; surface it as optional experimental module when enabled.

# 6. Graphics engine and resource management

## 6.1 Framegraph

Canonical pass graph (passes skip if not active):

```
Cue/Audio time
  -> Asset Preload / Scene State
  -> Raster depth+GBuffer / base scene
  -> [DXR lighting/reflection pass] -----------\
  -> [CUDA particles simulation/render] -------+-> HDR Composite
  -> [Tensor neural material texture] --------/
  -> [NPU latest async mood cues] (parameters, never direct pixels)
  -> [Optional NVENC/NVDEC feedback as texture]
  -> bloom / spectral chroma / grain / tone mapping
  -> HUD only if enabled
  -> swapchain Present + optional Spout sender
```

D3D12 resource barriers and access transition ownership are explicit. For CUDA exchange, use supported **external memory imports** and **shared D3D12 fences as CUDA external semaphores**. Prefer same physical adapter and GPU-to-GPU data paths. Recreate swapped resources on resize and handle device-lost procedures.

## 6.2 Default display/output

- Target 1920x1080 at 60 fps borderless/windowed fullscreen, vsync on by default. Alternative 720p/30 fallback.
- Internal linear HDR lighting e.g. `R16G16B16A16_FLOAT`, SDR sRGB tonemap/output first. HDR10/scRGB is a future milestone only if display/output and color management are tested.
- Support 16:9 primary; later 4:3/9:16 compositing and aspect-safe graphics for VJ performances. Do not stretch content.
- Separate **visual canvas** from diagnostics and transport overlays so Spout output can be pristine.
- Avoid the original interactive MouthLife “under 25 draw calls” blanket constraint; DXR dispatches, particle passes, and D3D12 compute are different workloads. Optimize draw/dispatch counts by measurement.

## 6.3 Nominal memory budget (16 GB reference GPU; targets, not guarantees)

| Category | Desired working budget |
|---|---:|
| Base raster scene, geometry, GBuffer, textures | 2.0 GB |
| DXR acceleration structures + reflection history | 2.5 GB |
| Tensor weights + activations + output textures | 2.0 GB |
| CUDA particle buffers + staging | 1.5 GB |
| Composite/history/postprocess + Spout buffers | 1.5 GB |
| Streaming/asset/transient reserve | 2.5 GB |
| **Target total** | **12.0 GB** |

Hard admission-control **soft cap 14 GB app-owned/estimated allocation** (with caution: reported CUDA/driver/resource usage may not map perfectly to true residency). Do not pre-allocate all peak budgets during a solo. Add runtime telemetry; on memory pressure evict unused scene textures, reduce history buffers, halve neural resolution, reduce particle count, and disable ray bounces in that order as artistically reasonable.

## 6.4 Frame-time budget at 60 fps

**16.67 ms wall-clock/frame**, including scheduling/presentation, and allowing overlaps only if measured:

| Pipeline unit | Target budget |
|---|---:|
| Base raster + geometry | 2.0 ms |
| DXR reflections | 3.5 ms |
| Neural texture update (not necessarily every frame) | amortized 1.5 ms |
| CUDA particle update/render | 2.5 ms |
| Composite/postprocess | 2.0 ms |
| Transfers/sync/driver/headroom | 3.0 ms |
| **Budgeted total** | **14.5 ms** |

These are **engineering targets**, not measured RTX 5080 benchmarks. The remaining ~2.17 ms headroom is insufficient for sudden shader compilation and media decode; precompile and do both asynchronously. NPU runs asynchronously at low rate, outside the render critical path.

## 6.5 Adaptive quality logic

- Capture GPU frame-time median and 95th percentile over sliding window.
- If p95 > ~18 ms for 2–3 consecutive windows: lower RT samples/bounces, tensor texture resolution/update rate, particle count, postprocess quality, then dynamic render resolution. Degrade gradually, not abruptly on the musical downbeat.
- If memory budget exceeded: *immediately* shed nonessential assets/effects based on pressure, not merely FPS.
- Restoration hysteresis: do not raise effects until sustained headroom; transitions only at safe musical/camera cuts when possible.
- Quality presets: **Safe** (720p/30, raster + CPU script), **Balanced** (1080p/60 adaptive), **Showcase** (1080p high), **4K Experimental** (manual, may run <60 fps). Safe preset should be available even on the reference hardware.

# 7. Timeline, data, determinism, and control

## 7.1 Film control

- States: Uninitialized -> Loading -> Playing / Paused / Seeking -> Ended / Loop -> Shutdown.
- Primary clock: audio sample position; silence-clock fallback; never time scenes by “frame number * 1/60” under variable frame rate.
- Seek must restore camera, emitter, material, director and media-feedback state. For stateful particle simulation, use periodic snapshots or fast deterministic resimulation from a checkpoint. If expensive, show a brief nonblocking transitional image while simulation catches up.
- Loop points are named and exact (audio sample boundaries). No clicks/pops at loop seams: use crossfade or naturally looped audio stems.
- Commands can come from keyboard, remote MIDI (future), or optional Xbox controller transport; there is **no character control**.
- Family/Midnight editions share timing, shot locations, and technology. Change dialog, text, wardrobe/presentation, and music stems where required without duplicating the whole runtime.

## 7.2 Suggested cue format

```json
{
  "schema": 1,
  "film_id": "boo-sah-sah-glass-orchid",
  "duration_samples": 17280000,
  "sample_rate": 48000,
  "seed": 266203,
  "edition": "family",
  "scenes": [
    {
      "id": "taipei_intro",
      "start_sample": 0,
      "end_sample": 2160000,
      "visual": "intro_taipei_to_enamel",
      "preferred_engine": "RT",
      "fallback_visual": "intro_raster",
      "reveal": "opening_scale_reveal"
    },
    {
      "id": "mirror_intelligence",
      "start_sample": 2160000,
      "end_sample": 4800000,
      "visual": "mirror_orchid",
      "preferred_engine": "RT",
      "fallback_visual": "mirror_probes"
    }
  ],
  "solo_loops": {
    "rt": [2160000, 4800000]
  }
}
```

The complete production cue file must cover all 17,280,000 samples, with no overlaps or gaps except deliberate compositing layers. A cue validator fails CI on invalid ranges, missing visual assets, invalid fallback references, and asset license omissions.

## 7.3 Real-time parameter ownership

The NPU director may adjust bounded scene variables (`bloomStrength`, `irisAngle`, `lightTension`, `spectralShift`, `cutAccent`) only when explicitly allowed by cue metadata. Timeline has final authority over reveal timing and global fade. All parameter changes have smoothing coefficients. User edits (quality/volume/edition) take precedence over the director.

# 8. Failure handling, substitution, and fault injection

## 8.1 Health-state contract

Every optional engine is in exactly one of: **Probing, Ready, ActiveAccelerated, ActiveSubstitute, ActiveDegraded, Unavailable, Faulted, Cooldown**. A resource/plugin can report detected but not operational; only a successfully executed test makes it operational.

Per-effect status retained through the film; one-time toast at scene boundary plus detailed overlay on demand. Do not flood the show with errors.

## 8.2 Required failure cases

| Fault | Required visible behavior | Technical fallback |
|---|---|---|
| NPU absent | Madame N and midpoint remain | OpenVINO CPU compatible model or scripted rules |
| NPU installed but model compile fails | Film starts; NPU disabled with reason | CPU model/scripted director |
| NPU inference stalls mid-film | Continue current mood, later switch gracefully | Last-good cue then scripted director |
| CUDA absent | Film and particles continue at reduced complexity | D3D12 HLSL compute or reduced CPU mode |
| Tensor GEMM unsupported | Neural Velvet visual remains | General compute or procedural texture |
| DXR unsupported | Mirror scene remains, flatter reflections | Raster/SSR/probes |
| Shader compile fails | Do not blackscreen | Precompiled fallback shader and safe scene |
| NVENC absent | Film exactly same duration | Shader signal effect; codec module inactive |
| NVDEC absent | No real compression loop | Shader signal effect |
| User file decode error | No app crash; show local media warning | Procedural default input |
| Camera denied | No retries/secret access | Procedural default input |
| Spout receiver absent | Show window continues | Window-only output |
| Audio unavailable | Silent show continues | High-resolution monotonic clock |
| VRAM pressure | Reduce quality, evict unused assets | Quality ladder |
| D3D12 device lost | Attempt bounded teardown/recreate; if unsuccessful show readable failure screen | Not falsely described as always recoverable |
| Missing content asset | Use deliberate placeholder shot/tone | Prepackaged fallback visual |

## 8.3 Fault-injection test console

Developer-only flags can disable any hardware engine (`--disable-npu`, `--disable-rt`, `--disable-cuda`, `--disable-tensor`, `--disable-nvenc`), inject inference timeout, media decode error, shader compile failure, and memory pressure. The unit/integration test verifies that scene/time advances and the backend label updates.

The release build has a user-readable **Hardware Orchestra** status screen with badges **Accelerated / Substitute / Degraded / Unavailable** and explanatory text; never expose raw exceptions to the viewer.

# 9. Performance instrumentation and truth in advertising

## 9.1 Runtime overlay

Display optionally, never superimposed on clean Spout output:

- Playback position, scene, quality preset, selected edition, deterministic seed.
- CPU/GPU models; active DXGI adapter; D3D12 feature level; DXR tier; CUDA device identity; OpenVINO devices; active inference backend.
- FPS, CPU frame prep, GPU frame time, p50/p95 frametimes, present stall count.
- App-reported VRAM allocation/residency estimate and memory pressure warning.
- RT dispatch/ray workloads; Tensor GEMM dimensions + timing; CUDA particle counts/timing; NPU inference latency and age; NVENC frame count/codec/latency only if genuinely active.
- Dropped frames, async processing queue depths, codec feedback queue delay, current fallback reasons.

## 9.2 Profiling tools

- D3D12 PIX / timestamp queries for actual GPU pass times, markers for passes and transitions.
- NVIDIA Nsight Graphics for DXR/raster scenes; Nsight Compute for CUDA kernels and Tensor-math instruction confirmation, compatible tool+GPU version required.
- OpenVINO device/runtime properties, inference logs and per-layer profiling where available; do not claim physical NPU utilization from a request timestamp alone.
- Windows GPU memory and process diagnostics for resource pressure; explicitly distinguish estimated allocations from actual total device memory.
- NVENC encode stats where API exposes them; no “encoder busy %” invention.

## 9.3 Reproducible benchmark methodology

1. Pin build SHA, model SHA, driver versions, SDK versions, render resolution, quality preset, seeds, edition, and audio section.
2. Warm shaders/effects before measured segment; separately report cold-start compilation.
3. Run at least 3 repeated 60-second passes; capture mean, median and p95/p99 frametime; tally quality downgrades and failures.
4. Record hardware solo per-engine results and mixed finale results **separately**.
5. For comparisons, make backend substitution visible and use approximately equivalent visual workload; note where perceptual equivalence cannot be guaranteed.
6. Output a local JSON/CSV diagnostic report **only when the user chooses Export Diagnostics**. Video recording is never needed.
7. No benchmark number goes into marketing copy until collected on the actual system.

# 10. Spout, OBS, Resolume and live performance

**Phase after stable film:** integrate Spout2 D3D12 sender with one user-selected name, e.g. `BooSahSah_Clean`. Send final composited GPU texture (no UI overlays) to compatible receiver. Detect sender/receiver failures and continue window playback. Test OBS Spout2 source and Resolume receiving versions manually. Spout is **texture sharing**, not NVENC recording. Sender frame pacing and GPU adapter choice must be measured; shared-texture synchronization is explicit.

Optional VJ transport: OSC/MIDI mappings (future); stable keyboard transport now; scene/solo mode selection from unobtrusive UI or launch arguments. No interactive tasks inside visuals. Use screen blanking/hidden controls and a clean startup show mode for stage use.

# 11. Asset pipeline and legal/security guidelines

- Source assets in `assets/` with provenance/license manifest and attribution requirements. Only distribute materials with compatible redistribution rights.
- Use original procedural score and original character designs; no copying existing film visuals, music cues, trademarked logos, or celebrity likenesses.
- Font coverage for Traditional Chinese verified; no shipped third-party font binaries without distribution permission.
- Archive locally generated textures/motion data where appropriate to reduce runtime load; no mandatory on-the-fly AI content download.
- Nonviolent, kindhearted, visually glamorous spy-noir; avoid fetishization or culturally inaccurate stylization.
- Local media file access scoped to explicitly selected paths, with input-size/decode limits; camera opt-in and visible status.
- Catch exceptions at plugin boundaries; validate lengths/frames before decoding; fuzz malformed media safely using test corpora, not intentionally corrupting GPU driver command streams.
- Logs should redact full user media paths where possible and never contain footage pixels, private frame extracts, or microphone content by default.

# 12. Build, testing and CI

## 12.1 Build modes

- **BASE_ONLY**: D3D12+raster+audio, with all optional SDKs excluded, must build and launch.
- **ENABLE_DXR**: DXR module, runtime-checked.
- **ENABLE_CUDA**: CUDA particle plugin; optional, not base dependency.
- **ENABLE_TENSOR**: cuBLASLt plugin, dependent on CUDA module presence but independently optional.
- **ENABLE_OPENVINO**: Intel NPU/CPU director plugin, independently optional.
- **ENABLE_NVENC**: optional codec plugin; defaults `OFF` until video-feedback milestone.
- **ENABLE_SPOUT**: optional D3D12 sender after stable renderer.

Build design must allow x64 baseline without NVIDIA toolkits installed. Runtime absence of an enabled plugin must not prevent baseline playback.

## 12.2 Minimum test families

**Unit:** cue ranges, sample clock, timecode arithmetic, quality ladder, backend selection, fallbacks, deterministic scripted director, parameter bounds, asset manifest.

**Integration:** adapter detection, D3D12 device, shader loads, RT tier detection and tiny dispatch, CUDA kernel with result check, cuBLASLt known matrix, OpenVINO model compile + one actual inference on requested device, media source defaults, resource fences.

**Fault injection:** all missing engine permutations that are practical (at least each single missing engine plus NPU/CUDA/RT failures together); model timeout, memory pressure, device lost, codec load failure, seek during failure, audio removal.

**Visual regression:** representative stills/video frames at fixed seeds and timestamps; perceptual threshold comparisons where nondeterminism is unavoidable. Validate no black frames/NaNs and no flashes beyond safe luminance guidelines.

**Performance:** film 1080p60 reference build, 720p30 Safe, loop for at least 30 min without increasing allocations, rapid seek, resize, alt-tab, Windows sleep/resume, OBS/Resolume coexistence.

**Accessibility:** subtitles, mute/volume, reduced flash mode, motion intensity reduction, contrast-safe captions, no unskippable strobe sequences.

CI: Windows x64 compile + unit tests on ordinary runners; GPU tests labeled `hardware` and run only on configured test hardware; do not claim CI verified GPU support when run without the relevant device.

# 13. Implementation milestones and acceptance gates

## M0 — Project skeleton & baseline window

**Deliver:** CMake x64 project, Windows window, D3D12 swapchain, test-color shader, audio clock or silent clock, logs, artifact manifest, Base-only build. **Pass:** opens/closes on Windows with no CUDA, OpenVINO, NVENC SDK installed; 10-minute stability smoke test.

## M1 — Taipei 60-second vertical slice (the first creative deliverable)

**Deliver:** A camera-directed Taipei-inspired scene, transition to an enormous enamel/glass surface, opening reveal, original soundtrack sketch, safe raster visual, optional DXR reflective structure. **Pass:** 60 s autonomous presentation; Play/Pause/Restart; 1080p raster safe mode; verified DXR path on reference device; measured frame time; silent fallback. Do not build the other acts yet.

## M2 — RT Solo and raster fallback

**Deliver:** polished ray-traced mirror corridor, adaptive ray quality, Solo loop and profile data. **Pass:** independently selectable RT Solo, true DXR dispatch confirmed and distinguishable raster fallback, no black screen when DXR disabled.

## M3 — CUDA Solo

**Deliver:** original particle effects, CUDA kernels, HLSL fallback, bounded memory. **Pass:** 100k stable particles and successful disable-CUDA fault injection; measured update/render timings.

## M4 — Tensor Solo

**Deliver:** local neural field, cuBLASLt FP16 path, procedural substitute, resource bridge. **Pass:** matrix output checksum test, representative Nsight evidence of tensor-math, solo visual, no GPU crash if module unavailable.

## M5 — Madame N / NPU Solo

**Deliver:** compact model, explicit OpenVINO NPU probe + inference test, CPU/scripted fallback, bound mood controls. **Pass:** successful NPU execution on reference environment (or transparent failure report with CPU/scripted fallback), film never stalls while compiling/failing model, NPU Solo visually coherent.

## M6 — Six-minute film + two editions

**Deliver:** seven time ranges, original score, three plot reveals, dental world medley, Family/Midnight variants, preauthored director beats. **Pass:** full 360.000 s playback + deterministic reveal positions, no missing cue intervals, clean exits from all scenes, no gameplay or violent content.

## M7 — Combined finale & stability

**Deliver:** combined scene, framegraph resource sync, adaptive quality, failover under load. **Pass:** user can disable any one accelerator while playing; soundtrack/timeline continue, backend status correct, sensible visual replacement, no runaway memory.

## M8 — Spout2 and live show polish

**Deliver:** clean Spout sender, OBS/Resolume compatibility test, diagnostic overlay toggle, keyboard transport, 30-min loop stability. **Pass:** a valid receiver sees motion and correct color; app continues if no receiver exists.

## M9 — Optional Signal Hijack

**Deliver:** user media input, artifact effects, optional **real** in-memory NVENC->NVDEC cycle, shader substitute, codec diagnostics. **Pass:** with NVENC disabled or nonexistent, Full Film + Finale Loop remain unaffected; codec effect never requires saving video; no media permission without action.

## M10 — Distribution

**Deliver:** signed installer *if code-signing certificate is available*, portable ZIP alternative, license manifest, changelog, build recipe, user guide, crash/log capture opt-in. **Pass:** clean Windows 11 x64 install runs base scene without development SDK; optional features discover their runtime dependencies and report them clearly.

# 14. Priorities, ownership and scope discipline

**P0 now:** M0 and M1; create an appealing visible film fragment.  
**P1:** M2 RT and M3 CUDA; these provide immediate demoscene spectacle.  
**P2:** M4 Tensor, M5 NPU; enforce true hardware verification.  
**P3:** M6 narrative integration, M7 combined finale.  
**P4:** M8 Spout and optional M9 NVENC.  
**P5:** Distribution, Arm64 port, additional edits and live media inputs.

Avoid building an unbounded plugin framework before first visible proof. Use simple explicit interfaces; refactor only when second backend warrants it.

# 15. First coding-agent assignment — copy/paste

> You are the lead C++/graphics engineer implementing BOO SAH SAH: The Glass Orchid Protocol. Read `docs/SPEC_v4.md` in full. This is a six-minute, noninteractive, nonviolent Taiwanese-inspired spy-tech demoscene application for Windows 11. Start with **M0 only**. Create a CMake C++20 Windows x64 D3D12 project with a swapchain, triangle/test visual, clean frame clock, settings parser, hardware capabilities report, diagnostics logging, and graceful handling of missing optional accelerators. The base build must not require CUDA, OpenVINO, NVENC, or Spout. Provide PowerShell configure/build/smoke scripts, unit tests for cue timeline arithmetic, and instructions to run the app. After compiling and executing all tests available in your environment, report exactly what passed, what could not be tested on that hardware, and what remains for M1. **Do not** generate the entire six-minute film, NPU model, CUDA simulation, or encoder module yet. Never substitute mock acceleration while labeling it real. Do not declare M0 complete without an actual build and passing relevant smoke tests.

# 16. Official reference links for implementation

- Microsoft D3D12 Hello World samples: https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-hello-world-samples-win32/
- Microsoft D3D12 DXR samples: https://learn.microsoft.com/en-us/samples/microsoft/directx-graphics-samples/d3d12-raytracing-samples-win32/
- DXR functional specification: https://microsoft.github.io/DirectX-Specs/d3d/Raytracing.html
- NVIDIA CUDA Direct3D interoperability: https://docs.nvidia.com/cuda/cuda-programming-guide/04-special-topics/graphics-interop.html
- Intel OpenVINO NPU guidance: https://docs.openvino.ai/2026/openvino-workflow/running-inference/inference-devices-and-modes/npu-device.html
- OpenVINO automatic device selection and fallback limitations: https://docs.openvino.ai/2026/openvino-workflow/running-inference/inference-devices-and-modes/auto-device-selection.html
- OpenVINO supported devices: https://docs.openvino.ai/2026/documentation/compatibility-and-support/supported-devices.html
- NVIDIA Video Codec SDK: https://developer.nvidia.com/video-codec-sdk
- NVIDIA NVENC programming guide: https://docs.nvidia.com/video-technologies/video-codec-sdk/13.0/nvenc-video-encoder-api-prog-guide/
- Spout2 D3D12 sender/receiver examples: https://github.com/leadedge/Spout2/blob/master/SPOUTSDK/SpoutDirectX/readme.md

**End of v4.0 master specification.**
