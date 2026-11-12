# BooSahSah Visual Rebuild, PBR/DXR Pipeline, Interactive RTX Studio & Verification Report

Verified locally on 2026-11-12 on branch `codex/film-quality-robustness` (starting from commit `3478b3c`).
- **Host:** Windows 11 (Build 26200), Intel Core Ultra 9 275HX (63 GB RAM), **NVIDIA GeForce RTX 5080 Laptop GPU** (16 GB VRAM, Driver 621.14, Feature Level 12_2, Shader Model 6.7, DXR Tier 1.2, CUDA Driver API 13.5), Intel AI Boost NPU, Blender 5.2.1 LTS, MSVC C++20 (`/W4`, DXC `-Ges -WX -O3`).
- **Baseline Preservation:** Pre-rebuild captures from `3478b3c` are preserved under `build/before_visual_rebuild/`, and timing receipts from `3478b3c` are preserved under `build/quality_after/timings/`.
- **D3D12 Debug Layer Availability:** `C:\Windows\System32\d3d12SDKLayers.dll` (Windows Graphics Tools) is **absent on this host**. All receipts honestly record `"debug_layer_active": false`, `null` error/warning counts, and `"warning_validation": "unavailable: Windows Graphics Tools debug layer is absent"`. Running `--require-debug-layer` rejects this host as expected (`build/quality_strict_debug_probe.log`). No clean D3D12 debug-layer run is claimed.

---

## 1. Asset, PBR/DXR Shading & Interactive NVIDIA-RTX Studio Architecture

Guided by NVIDIA’s *Marbles RTX* asset craftsmanship ([Substance 3D Magazine](https://magazine.substance3d.com/how-nvidias-marbles-rtx-demo-was-textured-with-substance/)) and interactive RTX showcase workflows (*Marbles RTX*, *RTXDI*, *NRD*)—metric real-world texel density, bevelled silhouette edges with face-weighted normals, physically grounded roughness/micro-imperfection breakup, unified ray-hit shading, live lighting/camera inspection, and progressive reference convergence—while preserving BooSahSah’s Taiwanese Glass Orchid and oral-care narrative:

1. **Runtime Texture Pipeline (`BTEX001`, `include/boo/btex.hpp`, `src/render/btex.cpp`, `tools/gen_pbr_textures.py`):**
   - **42 PBR Material Array Slices (`1024×1024`, 11 mip levels, 16× anisotropic filtering):**
     - `assets/textures/pbr_albedo_alpha.btex` (`DXGI_FORMAT_BC7_UNORM_SRGB`): sRGB base color + botanical cutout opacity mask.
     - `assets/textures/pbr_normal.btex` (`DXGI_FORMAT_BC5_UNORM`): linear tangent-space normal XY with orthonormal MikkTSpace-equivalent tangent/bitangent handedness reconstruction (`tangent.w` sign validated against UV gradient orientation).
     - `assets/textures/pbr_ormt.btex` (`DXGI_FORMAT_BC7_UNORM`): linear Ambient Occlusion (`R`), Toksvig mip-compensated Roughness (`G`, eliminating distant specular shimmer on grooved/brushed metals and tiles), Metallic (`B`), and Translucency / Clearcoat (`A`).
     - `assets/textures/pbr_ibl_env.btex` (`DXGI_FORMAT_R16G16B16A16_FLOAT`): GGX-prefiltered HDR environment arrays (misty Taipei Basin night/dawn skies + warm architectural interior bounce).
2. **Deterministic Blender 5.2 Asset Pipeline (`BMESH001`, `tools/gen_trilogy_meshes.py`):**
   - Metric world-scale UV projection, Face-Weighted Normals (FWN) with sharp-crease splitting, pure-Python bevelled rotational/extruded silhouettes (`closed_tube`, `revolve_closed_ring`, `superellipse_vessel`, `closed_blade`), and curated CC0 photogrammetry/scans & PBR textures from Poly Haven (`LICENSES.md`, `third_party/polyhaven_cc0/provenance.json`).
   - Verified **100% bit-identical SHA-256 determinism** across two independent Blender 5.2 headless runs across all 7 `.bmesh` kits (`599,394` total triangles, `0` nonmanifold edges):
     - `dawn_alcove.bmesh`: **94,700** triangles (`0` nonmanifold)
     - `taipei_101_hero.bmesh`: **157,128** triangles (`0` nonmanifold; grounded Xinyi district avenues, plaza, mountain ridges, and planted rooftop curbs)
     - `microbial_sanctuary.bmesh`: **145,976** triangles (`0` nonmanifold; machined brushed-brass/nickel optical aperture iris + stepped Calacatta marble alcoves, crazed celadon bowls, and solidified CC0 `Calathea orbifolia`, `Fern`, and `Potted Plant` scans)
     - `enamel_cathedral.bmesh`: **80,816** triangles (`0` nonmanifold; fluted hydroxyapatite columns on stepped marble plinths, bronze-framed concave mirror stelae, high-tessellation neural velvet lamellae)
     - `shanghai_trestle.bmesh`: **43,812** triangles (`0` nonmanifold)
     - `shenzhen_evtol.bmesh`: **42,806** triangles (`0` nonmanifold; pearl-clearcoated fuselage with twill carbon-fiber booms/skids/scimitar blades and titanium leading edges)
     - `glass_orchid_hero.bmesh`: **34,156** triangles (`0` nonmanifold — strictly inside `[24,000, 36,000]`)
3. **D3D12 / DXR 1.1 Renderer & GDDR7 VRAM Geometry Upgrades (`src/render/trilogy_renderer.cpp`, `assets/shaders/m2_mesh.hlsl`):**
   - **GDDR7 `D3D12_HEAP_TYPE_DEFAULT` Vertex/Index Buffer Promotion:** Moved the `93.5 MB` combined `.bmesh` vertex (`86.3 MB`) and index (`7.2 MB`) buffers off system-RAM `D3D12_HEAP_TYPE_UPLOAD` onto dedicated GPU VRAM (`D3D12_HEAP_TYPE_DEFAULT`) via a one-time GPU upload during initialization. Eliminating PCIe fetches during DXR `RayQuery` vertex/UV attribute shading increased ray throughput up to `5.51 GRays/s` (`5511.6 MRays/s` on `355s` Dawn Penthouse; `1809.3 MRays/s` on `318s` Grand Tutti) and brought **every scene across the 360-second film under `4.66 ms p50` at native 1440p with `0` quality downgrades**.
   - **Hardware Depth Prepass (`m2_depth_cutout_ps.cso` + `DepthFunc = EQUAL`):** Eliminates all back-to-front multi-kit and multi-layer foliage DXR overdraw (`84.8M -> 19.4M` rays/frame at 1440p `355s`).
   - **Consistent Direct & Ray-Hit Material Evaluation:** Secondary reflection/transmission rays fetch full `BC7`/`BC5` PBR textures at ray-cone mip LODs and evaluate GGX + IBL + sun lighting consistently with primary hits.
   - **Alpha-Cutout Foliage with Matching Ray-Query Visibility:** `mat 29` (`Calathea`) and `mat 36` (`Fern`) use `D3D12_RAYTRACING_GEOMETRY_FLAG_NONE` BLASes with matching `a >= 0.44` candidate commit logic across primary visibility, soft sun shadow rays (`traceShadow`), mirror rays (`traceMirrorScene`), and sharp reflection/refraction cones (`coneSpread < 0.045`), while diffuse GI/AO and rough cones use hardware `RAY_FLAG_FORCE_OPAQUE` traversal.
   - **Motivated Dawn Illumination, Soft Shadows, Hemisphere GI & Mirror-to-Glass Transmission:** Stratified Vogel-disk soft sun shadows, cosine-weighted DXR hemisphere GI + multi-scale contact AO (`4 mm` crevices to `3.85 m` window portal), AR-coated architectural float glass (`mat 40`), and `traceMirrorScene` allowing the flat vanity mirror (`mat 34`) to see cleanly through the celadon-tinted glass shower enclosure (`mat 4`) to the mosaic shower wall and rain showerhead behind the camera.
4. **Interactive NVIDIA RTX Explore Camera, NRD v4.18.0 Reference Accumulation & RTXDI Studio (`src/render/nrd_bridge.cpp`, `src/app/main.cpp`, `assets/shaders/m1_taipei_slice.hlsl`):**
   - **6-DOF Free-Fly Camera & 6 Curated Hero Presets (`F`, RMB, `WASDQE`, Mouse Wheel FOV, XInput Gamepad, `F2–F7`, `--explore`, `--explore-preset 1..6`):** Smoothly detaches from the live film camera with `Shift`/`Ctrl`/`Alt` speed modifiers (down to millimeter macro inspection of water droplets and leaf venation) and eases back to the exact deterministic film camera over `0.65 s` when exited.
   - **Dynamically Loaded NVIDIA Real-Time Denoisers (`nrd/NRD.dll` v4.18.0):** Built from official `NVIDIA-RTX/NRD` (`scripts/build_nrd_sidecar.ps1`) and loaded strictly via `LoadLibraryExW` + `GetProcAddress` (zero static PE import). Stationary Explore views automatically engage `nrd::Denoiser::REFERENCE` DXIL compute accumulation up to `4096 SPP` with sub-pixel Halton `[2,3]` camera jitter, 2-bounce hemisphere GI, stochastic GGX glossy reflections, internal glass dispersion/reflection, and stationary Tensor/CUDA buffer reuse (gracefully falling back to the built-in D3D12 HLSL RGBA32F accumulator if `NRD.dll` is absent).
   - **NVIDIA RTXDI Reservoir Sampling (`RTXDI_DIReservoir` / ReSTIR DI), Live Sun Orbit & Draggable RTX Split Wipe:** Adapted `RTXDI_DIReservoir` reservoir importance sampling (`LICENSES.md`) evaluates 4 physical practical emitters (2700K recessed ceiling vanity downlight, 2900K backlit mirror perimeter halo, 2700K opal cylindrical wall sconce / under-cabinet wash, and emerald-jade bioluminescent keepsake Glass Orchid core) with 1 unbiased DXR shadow ray (`J` or Studio Bar `LAMP` toggle). Users can orbit sun elevation (`Golden Dawn` -> `High Day` -> `Midnight`) and azimuth live (`Alt+LMB` drag, `[`/`]`, or Studio Bar `SUN`/`AZM` sliders) and drag a live `5%–95%` **RTX ON / Raster OFF** split-screen wipe (`H` or `WIPE`) strictly in Pass 4 without invalidating converged `SPP` or polluting Pass-3 Spout2 output.

---

## 2. Visual Proof, Cycles Reference Comparison, Interactive Showcase & 10s Hero Move

All visual proof artifacts are generated at native **2560×1440** with `--no-activate`:
- **Blender 5.2 Cycles Path-Traced Reference Renders (`tools/render_cycles_reference.py`):**
  - `build/visual_proof/dawn_cycles_342s.png` and `build/visual_proof/dawn_cycles_355s.png` (rendered from the exact `.bmesh` geometry, PBR `.btex` source maps, sun angle, and timeline camera states).
- **Before (`3478b3c`) vs. Cycles Reference vs. Real-Time D3D12/DXR Proof Sheets (`tools/build_visual_proof_sheets.py`):**
  - `build/visual_proof/dawn_before_cycles_after_comparison.png` (3-way 1440p Dawn Bathroom comparison at `342.0s` and `355.0s`)
  - `build/visual_proof/dawn_1to1_crops_sheet.png` (5 unscaled 1:1 native pixel crops comparing D3D12/DXR vs. Cycles across the porcelain basin undercut/CNC faucet/3D water droplets, flat mirror reflection, Calathea leaf venation/translucency, waffle towels/amber soap/toothbrush bristles/keepsake orchid, and AR float window/Taipei 101 vista)
  - `build/visual_proof/full_film_before_after_sheet.png` (Before vs. After across Acts 1, 2, 3, 5, and 7)
- **Recorded 10-Second Hero Camera Move (`342.0s -> 352.0s`, 300 frames @ 30 fps, native 2560×1440):**
  - `build/visual_proof/dawn_hero_10s_move.mp4` (and raw PNG sequence `build/visual_proof/hero_10s_frames/frame_0000..0299.png`), verified free of clipping, temporal popping, shadow breakup, or quality downgrades (`4.45 ms p50 / 5.04 ms p95` at 1440p Showcase).
- **Interactive NVIDIA RTX Showcase Captures (`build/interactive_captures/`, `--interactive-captures`):**
  - `interactive_preset2_faucet_64spp_studio.png` & `interactive_preset2_faucet_64spp_clean.png` (64-SPP NRD v4.18.0 CNC faucet, knurled collar, porcelain overflow & 3D water droplets macro + Pass-4 Studio Bar)
  - `interactive_preset3_calathea_64spp_clean.png` & `interactive_preset4_orchid_vase_64spp_clean.png` (64-SPP NRD macro views of backlit `Calathea orbifolia` venation and keepsake Glass Orchid refractive dispersion)
  - `interactive_bathroom_golden_dawn_32spp.png`, `interactive_bathroom_high_day_32spp.png`, `interactive_bathroom_midnight_rtxdi_lamps_on_32spp.png`, and `interactive_bathroom_midnight_lamps_off_32spp.png` (live Sun time-of-day & RTXDI 2700K practical lamp sweep)
  - `interactive_bathroom_rtx_split_wipe_studio.png` (draggable Pass-4 RTX ON / Raster OFF split curtain) and `interactive_showcase_montage.png`
- **Full-Film & Telemetry Captures (`build/film_captures/`):**
  - `film_family_montage.png`, `film_midnight_montage.png`, `solos_montage.png`, `film_family_355s_presentation.png`, and `film_family_355s_stats_layout.png` (confirming `台北 / TAIPEI` + `BOO SAH SAH` clear of both `LIVE GPU` and `BENCH [B]` panels, `[F:EXPLOR]` interactive pill in Pass 4, and clean Pass-3 Spout isolation).

---

## 3. Candid Assessment Against NVIDIA’s *Marbles RTX* Reference & Remaining Weaknesses

### Where BooSahSah Approaches the *Marbles RTX* Standard
- **Close-Up Hero Materiality & Bevelled Silhouettes:** In the Dawn Penthouse Bathroom (`342s–360s`), Enamel Cathedral (`95s`), and Botanical Sanctuary (`175s`), close-up props survive 1:1 native 1440p crop and interactive macro inspection: the smooth double-walled ceramic vessel basin with recessed chrome overflow/drain, CNC brushed-nickel faucet with knurled collar and 3D water droplets, directional smoked-walnut cabinetry grain, fluted finger-mosaic tiles with grout normal crevices, scanned `Calathea orbifolia` leaves with herringbone venation and backlit subsurface glow, folded waffle-weave washcloths, amber glass apothecary bottle, and optical mirror reflections closely parallel the Blender 5.2 Cycles path-traced reference.
- **NVIDIA-Demo Interactivity & Progressive Reference Convergence:** Pressing `F` (or launching `--explore-preset 1..6`) delivers smooth 6-DOF inspection, live sun elevation/azimuth scrubbing from Golden Dawn to Midnight, toggleable RTXDI 2700K practical emitters, a draggable RTX ON / Raster OFF split curtain, and automatic `NRD.dll` v4.18.0 `REFERENCE` convergence (`3.63–4.92 ms/frame` at 1440p) with stochastic 2-bounce GI, glossy GGX cones, and internal glass reflections.
- **Coherent Real-Time Motion & GPU Headroom:** With geometry promoted to GDDR7 `D3D12_HEAP_TYPE_DEFAULT` VRAM, Toksvig roughness mip-mapping, 16× anisotropic filtering, IGN-stratified soft shadows/GI, and the hardware Z-prepass, every scene across the 360s film runs at **`1.60–4.66 ms p50`** at native 1440p (`>210 fps`, `0` quality downgrades) without temporal denoiser ghosting during fast motion or seeks.

### Candid Remaining Gaps Relative to *Marbles RTX*
1. **Real-Time Motion (`1 SPP`) vs. Stationary NRD Reference (`64–4096 SPP`):** During canonical real-time playback (`1 SPP`), BooSahSah uses deterministic single-bounce DXR hemisphere GI + prefiltered IBL and single-hit thin-walled glass to guarantee zero temporal ghosting and bit-exact Release == Debug frame determinism, whereas 2-bounce GI and internal glass reflections engage during stationary Explore NRD convergence. Consequently, in `1 SPP` motion frames (`dawn_before_cycles_after_comparison.png`), Cycles still exhibits deeper multi-bounce shadow contrast on the left Venetian plaster wall and denser multi-bounce glass darkening in the amber bottle than the real-time `1 SPP` approximation.
2. **Exterior Urban Background Detail vs. Interior Hero Props:** While Taipei 101 and Xinyi District are grounded on avenues, podiums, streetlamps, and basin mountain ridges, mid-rise background towers still rely on procedural architectural facade grids rather than hero-grade photogrammetry/kitbashed street clutter.
3. **Raster Fallback Mode (`--disable-rt` / Right Side of `WIPE`):** Uses identical PBR meshes, normal maps, textures, and prefiltered IBL, substituting environment lookups where DXR reflections, refractions, and ray-traced shadows are disabled.
4. **D3D12 Debug Layer Unverified on This Host:** `d3d12SDKLayers.dll` is not installed on this machine; strict `--require-debug-layer` validation remains required on a machine with Windows Graphics Tools enabled.

---

## 4. Functional, Stress, GPU Timing, VRAM & Packaging Evidence

| Gate | Release | Debug |
|---|---|---|
| C++20 / HLSL DXC `-Ges -WX` build | Pass (`0` warnings) | Pass (`0` warnings) |
| Unit suite (`BooUnitTests`) | Pass (`18/18`) | Pass (`18/18`) |
| Hardware integration suite (`BooHardwareIntegrationTests`) | Pass (`18/18`) | Pass (`18/18`) |
| Cue & provenance manifest (`25` SHA-256 entries) | Pass | Pass |
| Mesh integrity & 2-run bit-identical `.bmesh` determinism | Pass (`7/7` kits, `599,394` tris) | Same |
| Zero optional-SDK PE static-import audit (`dumpbin /dependents`, incl. `NRD`/`XInput`) | Pass | Pass |
| Smoke (`M10` Spout, 16:10 letterbox, all-fallbacks, device recovery, `M2`/`M1`, NPU Solo) | Pass | Pass |
| Trilogy native 1440p captures & determinism (`--trilogy-captures`) | Pass | — |
| Interactive RTX Showcase 1440p NRD/RTXDI captures (`--interactive-captures`) | Pass | — |
| Full-Film native 1440p captures (`--film-captures`) | Pass (`72` deterministic + `2` stats PNGs) | Byte-identical (`72/72`) |
| 10-second Dawn Hero 1440p video capture (`--hero-video-capture`) | Pass (`300/300` frames) | — |
| Full-Film 224-iteration stress & recovery (`--film-stress`) | Pass (`224/224`) | Pass (`224/224`) |
| GPU retirement & Spout2 live D3D11 receiver (`--lifecycle-tests`) | Pass (`2/2`) | — |
| Single-file LZMS installer (`BooSahSah-Setup-win64.exe`, incl. `nrd\NRD.dll`) + GUI check | Pass | — |
| D3D12 debug-layer warnings/errors (`--require-debug-layer`) | Unavailable (`d3d12SDKLayers.dll` absent) | Unavailable |

### Stress Memory & Lifecycle Peaks (224 Iterations across All 7 Acts)

| Metric (Warmup Peak `0..31` / Subsequent Peak `32..223`) | Release | Debug |
|---|---:|---:|
| DXGI Process Local VRAM | `900.4 / 900.4 MiB` (`0.0 MiB` growth) | `900.4 / 900.4 MiB` (`0.0 MiB` growth) |
| Private Bytes | `1701.3 / 1727.6 MiB` | `1716.9 / 1725.7 MiB` |
| Working Set | `828.8 / 731.6 MiB` | `831.4 / 737.4 MiB` |
| Process Handles | `1575 / 1580` | `1571 / 1579` |
| Max Adjacent 48 kHz Audio Sample Step | `0.00213893` (`< 0.010`) | `0.00213893` (`< 0.010`) |
| Simulated Device-Loss & Recreate Time | `3.45 s` | `4.12 s` |

### Sequential GPU Timings (`p50 / p95` ms) & Peak VRAM (`RTX 5080 Laptop GPU`)

Measured sequentially (`--no-activate --width 2560 --height 1440 --mute`) over 3-second playback windows (plus sustained 10-second Dawn Bathroom camera-move windows `342.0s -> 352.0s` and Interactive Explore NRD v4.18.0 convergence windows). `Before` values are commit `3478b3c` (`286,308` untextured tris on PCIe `UPLOAD` heap); `Rebuild` values are the full `599,394`-tri, `42`-material PBR + DXR GI/soft-shadow/RTXDI/NRD build on GDDR7 `DEFAULT` VRAM (`docs/quality_timings.json`). Quality downgrades = **`0` on every row**.

| Sample / Window | `balanced` (1080p) `3478b3c` | `balanced` (1080p) Rebuild | `showcase` (1440p) `3478b3c` | `showcase` (1440p) Rebuild | Rebuild VRAM (`1080p / 1440p`) |
|---|---:|---:|---:|---:|---:|
| `36s` (Act I Glass Orchid) | `2.66 / 3.02` | `1.72 / 2.18` | `3.09 / 3.36` | `2.63 / 2.91` | `787.5 / 896.6 MiB` |
| `95s` (Act II Enamel Cathedral) | `5.06 / 5.44` | `1.96 / 2.53` | `7.09 / 8.19` | `3.03 / 3.53` | `787.5 / 896.6 MiB` |
| `175s` (Act III Botanical Sanctuary) | `3.58 / 3.90` | `2.83 / 3.24` | `4.23 / 4.60` | `4.05 / 4.50` | `787.5 / 896.6 MiB` |
| `225s` (Act IV CUDA Mutiny) | `2.76 / 3.11` | `1.85 / 2.29` | `3.58 / 3.90` | `3.00 / 3.46` | `787.5 / 896.6 MiB` |
| `260s` (Act V Shenzhen eVTOL) | — | `1.19 / 1.64` | — | `1.60 / 2.06` | `806.0 / 915.1 MiB` |
| `275s` (Act VI Combined Sanctuary) | `7.74 / 8.86` | `2.81 / 3.12` | `9.97 / 10.98` | `4.08 / 4.58` | `787.5 / 896.6 MiB` |
| `318s` (Act VI Grand Tutti Climax) | `7.82 / 8.93` | `2.88 / 3.36` | `11.14 / 11.77` | `4.13 / 4.57` | `787.5 / 896.6 MiB` |
| `342s` (Act VII Dawn Vanity Reveal) | `2.67 / 3.03` | `2.99 / 3.37` | `3.22 / 3.53` | `4.66 / 5.09` | `787.5 / 896.6 MiB` |
| `355s` (Act VII Dawn Penthouse Settle) | `2.91 / 3.25` | `2.91 / 3.33` | `3.63 / 3.97` | `4.38 / 4.73` | `787.5 / 896.6 MiB` |
| **Hero 10s Move (`342.0s -> 352.0s`)** | — | **`2.93 / 3.33`** (`557` f) | — | **`4.45 / 5.04`** (`555` f) | `787.5 / 896.6 MiB` |
| **Explore #1 Dawn Wide (NRD v4.18.0)** | — | **`2.38 / 2.66`** (`174` f) | — | **`3.63 / 4.14`** (`174` f) | `787.5 / 896.6 MiB` |
| **Explore #2 Faucet Macro (NRD v4.18.0)** | — | **`3.16 / 3.91`** (`174` f) | — | **`4.92 / 5.33`** (`174` f) | `787.5 / 896.6 MiB` |

---

## 5. Reproduction Instructions

```powershell
# 1. Regenerate PBR .btex texture arrays, deterministic .bmesh kits, Cycles references, NRD sidecar, and SHA-256 manifest
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python tools/gen_pbr_textures.py
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python tools/gen_trilogy_meshes.py
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python tools/render_cycles_reference.py
.\scripts\build_nrd_sidecar.ps1
.\.venv\Scripts\python.exe tools/verify_trilogy_meshes.py
.\.venv\Scripts\python.exe tools/refresh_asset_hashes.py --check

# 2. Build & run Release + Debug smoke, unit, CTest, and PE zero-optional-SDK gates (always with --no-activate)
.\scripts\run_smoke.ps1 -Preset x64-base-release
.\scripts\run_smoke.ps1 -Preset x64-base-debug -SkipBenchmarks

# 3. Capture native 1440p Trilogy, Interactive RTX Showcase, Full-Film, 10s Dawn Hero Move video, and Visual Proof sheets
.\build\x64-release\boo_integration_tests.exe --trilogy-captures --no-activate
.\build\x64-release\boo_integration_tests.exe --interactive-captures --no-activate
.\build\x64-release\boo_integration_tests.exe --film-captures --no-activate --capture-dir build/film_captures
.\build\x64-debug\boo_integration_tests.exe --film-captures --no-activate --capture-dir build/film_captures_debug
.\build\x64-release\boo_integration_tests.exe --hero-video-capture --no-activate --capture-dir build/visual_proof
ffmpeg -y -framerate 30 -i build/visual_proof/hero_10s_frames/frame_%04d.png -c:v libx264 -crf 15 -pix_fmt yuv420p build/visual_proof/dawn_hero_10s_move.mp4
python tools/build_visual_proof_sheets.py

# 4. Launch interactively in 1440p Showcase Explore Studio (e.g., Preset 2 CNC Faucet & Droplets Macro; omit --no-activate when driving interactively)
.\build\x64-release\boo_sah_sah.exe --width 2560 --height 1440 --preset showcase --explore-preset 2

# 5. Run 224-iteration Stress & Lifecycle suites
.\build\x64-release\boo_integration_tests.exe --film-stress --no-activate --capture-dir build/quality_stress_release
.\build\x64-debug\boo_integration_tests.exe --film-stress --no-activate --capture-dir build/quality_stress_debug
.\build\x64-release\boo_integration_tests.exe --lifecycle-tests --no-activate

# 6. Package & transaction-verify single-file Windows installer + GUI worker/cancel check
.\scripts\package_release.ps1
.\scripts\test_installer_gui.ps1 -SetupPath build/dist/BooSahSah-Setup-win64.exe
```
