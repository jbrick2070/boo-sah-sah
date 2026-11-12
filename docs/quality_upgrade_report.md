# BooSahSah Visual Rebuild, PBR/DXR Pipeline, Interactive RTX Studio & Verification Report

Verified locally on 2026-11-12 on branch `codex/film-quality-robustness` (starting from commit `3478b3c`).
- **Host:** Windows 11 (Build 26200), Intel Core Ultra 9 275HX (63 GB RAM), **NVIDIA GeForce RTX 5080 Laptop GPU** (16 GB VRAM, Driver 621.14, Feature Level 12_2, Shader Model 6.7, DXR Tier 1.2, CUDA Driver API 13.5), Intel AI Boost NPU, Blender 5.2.1 LTS, MSVC C++20 (`/W4`, DXC `-Ges -WX -O3`).
- **Baseline Preservation:** Pre-rebuild captures from `3478b3c` are preserved under `build/before_visual_rebuild/`, and timing receipts from `3478b3c` are preserved under `build/quality_after/timings/`.
- **D3D12 Debug Layer Availability:** `C:\Windows\System32\d3d12SDKLayers.dll` (Windows Graphics Tools) is **absent on this host**. All receipts honestly record `"debug_layer_active": false`, `null` error/warning counts, and `"warning_validation": "unavailable: Windows Graphics Tools debug layer is absent"`. Running `--require-debug-layer` rejects this host as expected (`build/quality_strict_debug_probe.log`). No clean D3D12 debug-layer run is claimed.

---

## 1. Asset, PBR/DXR Shading, Motion Denoising & Interactive *Marbles RTX* Studio Architecture

Guided by NVIDIA’s *Marbles RTX* asset craftsmanship ([Substance 3D Magazine](https://magazine.substance3d.com/how-nvidias-marbles-rtx-demo-was-textured-with-substance/)) and interactive RTX showcase workflows (*Marbles RTX*, *RTXDI*, *NRD*)—metric real-world texel density, bevelled silhouette edges with face-weighted normals, physically grounded roughness/micro-imperfection breakup, unified ray-hit shading, live buffer/AOV inspection, rigid-body marble physics, real-time motion denoising, and progressive reference convergence—while preserving BooSahSah’s Taiwanese Glass Orchid and oral-care narrative:

1. **Runtime Texture Pipeline (`BTEX001`, `include/boo/btex.hpp`, `src/render/btex.cpp`, `tools/gen_pbr_textures.py`):**
   - **42 PBR Material Array Slices (`1024×1024`, 11 mip levels, 16× anisotropic filtering):**
     - `assets/textures/pbr_albedo_alpha.btex` (`DXGI_FORMAT_BC7_UNORM_SRGB`): sRGB base color + botanical cutout opacity mask.
     - `assets/textures/pbr_normal.btex` (`DXGI_FORMAT_BC5_UNORM`): linear tangent-space normal XY with orthonormal MikkTSpace-equivalent tangent/bitangent handedness reconstruction (`tangent.w` sign validated against UV gradient orientation).
     - `assets/textures/pbr_ormt.btex` (`DXGI_FORMAT_BC7_UNORM`): linear Ambient Occlusion (`R`), Toksvig mip-compensated Roughness (`G`, eliminating distant specular shimmer on grooved/brushed metals and tiles), Metallic (`B`), and Translucency / Clearcoat (`A`).
     - `assets/textures/pbr_ibl_env.btex` (`DXGI_FORMAT_R16G16B16A16_FLOAT`): GGX-prefiltered HDR environment arrays (misty Taipei Basin night/harbor skies + CC0 `qwantani_dawn_puresky_1k` east-window vista and grounded indoor hemisphere).
2. **Deterministic Blender 5.2 Asset Pipeline (`BMESH001`, `tools/gen_trilogy_meshes.py`):**
   - Metric world-scale UV projection, Face-Weighted Normals (FWN) with sharp-crease splitting, pure-Python bevelled rotational/extruded silhouettes (`closed_tube`, `revolve_closed_ring`, `superellipse_vessel`, `closed_blade`), and curated CC0 photogrammetry/scans & PBR textures from Poly Haven (`LICENSES.md`, `third_party/polyhaven_cc0/provenance.json`).
   - Verified **100% bit-identical SHA-256 determinism** across two independent Blender 5.2 headless runs across all 7 `.bmesh` kits (`599,394` static `.bmesh` triangles + `18,432` procedural runtime MikkTSpace sphere template triangles = `617,826` loaded triangles, `0` nonmanifold edges):
     - `dawn_alcove.bmesh`: **94,700** triangles (`0` nonmanifold)
     - `taipei_101_hero.bmesh`: **157,128** triangles (`0` nonmanifold; grounded Xinyi district avenues, plaza, mountain ridges, and planted rooftop curbs)
     - `microbial_sanctuary.bmesh`: **145,976** triangles (`0` nonmanifold; machined brushed-brass/nickel optical aperture iris + stepped Calacatta marble alcoves, crazed celadon bowls, and solidified CC0 `Calathea orbifolia`, `Fern`, and `Potted Plant` scans)
     - `enamel_cathedral.bmesh`: **80,816** triangles (`0` nonmanifold; fluted hydroxyapatite columns on stepped marble plinths, bronze-framed concave mirror stelae, high-tessellation neural velvet lamellae)
     - `shanghai_trestle.bmesh`: **43,812** triangles (`0` nonmanifold)
     - `shenzhen_evtol.bmesh`: **42,806** triangles (`0` nonmanifold; pearl-clearcoated fuselage with twill carbon-fiber booms/skids/scimitar blades and titanium leading edges)
     - `glass_orchid_hero.bmesh`: **34,156** triangles (`0` nonmanifold — strictly inside `[24,000, 36,000]`)
3. **D3D12 / DXR 1.1 Renderer, 2-Interface Dielectric Glass & 3-Tier Denoising (`src/render/trilogy_renderer.cpp`, `assets/shaders/m2_mesh.hlsl`):**
   - **GDDR7 `D3D12_HEAP_TYPE_DEFAULT` Vertex/Index Buffer Promotion & Hardware Depth Prepass:** Geometry buffers reside in dedicated GDDR7 GPU VRAM with a cutout-aware Z-prepass (`m2_depth_cutout_ps.cso` + `DepthFunc = EQUAL`), keeping every scene across the 360-second film at **`3.94–7.57 ms p50`** at native `2560×1440` (`0` quality downgrades).
   - **Lambert-Normalized `8.5 m` Indoor DXR Hemisphere GI & Grounded Room Metals:** Extended `traceBathroomGiRay` (`TMax = 8.50 m`) so west/rear-directed indoor rays hit the enclosing bathroom walls rather than escaping into sky radiance, grounded indoor `mat 23` brushed champagne nickel to 100% ray-traced indoor reflections (`rough <= 0.62`), localized the `2700 K` opal sconce on secondary mirror hits (`traceSceneEx`), and restricted dawn aerial mist to low basin elevations so upper Taipei 101 stays crisp.
   - **Solid 2-Interface Snell Refraction, 3D Beer-Lambert Optical Density & Colored Dielectric Caustics:** Solid glass vessels (`mat 35` cognac-amber `exp(-float3(5.8, 34.0, 96.0) * d)`, `mat 4` emerald-celadon, `mat 3` crystal droplets & marbles) trace entry Snell refraction, internal `0x02u` 3D thickness rays, back-interface exit Snell refraction / Total Internal Reflection, and `traceShadowTransmittance` so curved glass casts Fresnel rim shadows and focused colored Beer-Lambert caustics onto the backsplash, marble vanity, and porcelain basin.
   - **Three-Tier Denoising Architecture (7-MRT Demodulated Radiance Pipeline):**
     1. **Canonical Film Playback (`PSDeterministicResolve`):** Stateless 17-tap normal/viewZ/roughness edge-stopping cross-bilateral filter on demodulated diffuse GI (`diffRadHit`) and specular radiance (`specRadHit`), eliminating 1-SPP IGN speckle on plaster, grout, and marble while preserving **100% seek repeatability** and **`72/72` Release == Debug PNG byte-identity**.
     2. **Interactive Live Motion (`nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR`):** Dynamically loaded `NRD.dll` v4.18.0 (`CSPackNrdReblur` + `NRD_NormalEncoding::RGBA8_UNORM` + `NRD_HitDistanceReconstructionMode::AREA_3X3` + 3D world motion vectors `worldMv` + `PSUnpackNrdReblur`) denoises 1-SPP indirect GI, RTXDI lights, and glossy reflections in real time while flying, scrubbing sun angles, or rolling physics marbles (`activeDenoiseMode() == 1`, `RBLR` Studio Bar badge).
     3. **Interactive Stillness (`nrd::Denoiser::REFERENCE`):** Automatically hands off to `NRD::REFERENCE` DXIL compute accumulation (`1 -> 4096 SPP`, `activeDenoiseMode() == 2`, `NRD` Studio Bar badge) with Halton `[2,3]` jitter once camera, sun, and marbles come to rest.
4. **Interactive *Marbles RTX* Physics, 8-Mode AOV Inspection & RTXDI Studio (`src/app/main.cpp`, `src/render/nrd_bridge.cpp`, `assets/shaders/m1_taipei_slice.hlsl`):**
   - **Interactive Ray-Traced Physics Marbles (`Y` / `F10`, `M` in Explore, `Shift+Y` / `Shift+M` clear, Gamepad `RB`, Studio Bar `MRBL`):** Spawns 10 rigid-body spheres (`8×` sub-stepped gravity, exact superellipse porcelain vessel bowl heightfield + finite-difference surface normals, cylindrical fixture collisions, elastic sphere-sphere collisions, rolling quaternion spin, and 3D `prevPos` motion vectors) across 6 procedural MikkTSpace sphere BLASes (`{3, 4, 35, 8, 23, 31}`) dynamically inserted into the DXR TLAS.
   - **8-Mode Live Buffer / AOV Inspection (`I` / `F9`, `Shift+I` reset, Gamepad `LB`, Studio Bar `AOV`):** Cycles `0: Final`, `1: Base Color`, `2: World Normal`, `3: Roughness/Metallic/AO`, `4: Direct + Caustics + RTXDI`, `5: Indirect Diffuse GI`, `6: Reflections & Refraction`, and `7: DXR Ray Heatmap` (bypassing 3D particles, bloom, vignette, and filmic distortion on raw data AOVs while keeping Pass-4 `AOV:` badges out of Pass-3 `CanvasClean`).
   - **6-DOF Camera, 6 Hero Presets (`F2–F7`), Live Sun Orbit, RTXDI Practical Toggle (`J`) & Draggable RTX Split Curtain (`H`):** Preserved and integrated into the widened Pass-4 Studio Bar (`432 * u`), with zero contamination of Pass-3 Spout2 output and bit-exact restoration of canonical film frames on exit.

---

## 2. Visual Proof, Cycles Reference Comparison, Interactive Showcase & 10s Hero Move

All visual proof artifacts are generated at native **2560×1440** with `--no-activate`:
- **Blender 5.2 Cycles Path-Traced Reference Renders (`tools/render_cycles_reference.py`):**
  - `build/visual_proof/dawn_cycles_342s.png` and `build/visual_proof/dawn_cycles_355s.png` (rendered from the exact `.bmesh` geometry, PBR `.btex` source maps, sun angle, and timeline camera states).
- **Before (`3478b3c`) vs. Cycles Reference vs. Real-Time D3D12/DXR Proof Sheets (`tools/build_visual_proof_sheets.py`):**
  - `build/visual_proof/dawn_before_cycles_after_comparison.png` (3-way 1440p Dawn Bathroom comparison at `342.0s` and `355.0s`)
  - `build/visual_proof/dawn_1to1_crops_sheet.png` (5 unscaled 1:1 native pixel crops comparing D3D12/DXR vs. Cycles across Crop A CNC faucet/basin undercut/3D droplets, Crop B mirror back-wall oak slats/leaf relief/sconce, Crop C scanned `Calathea` venation/translucency, Crop D waffle towels/cognac-amber soap/celadon vase/keepsake orchid, and Crop E east dawn window/Taipei 101 vista)
  - `build/visual_proof/full_film_before_after_sheet.png` (Before vs. After across Acts 1, 2, 3, 5, and 7)
- **Recorded 10-Second Hero Camera Move (`342.0s -> 352.0s`, 300 frames @ 30 fps, native 2560×1440):**
  - `build/visual_proof/dawn_hero_10s_move.mp4` (and raw PNG sequence `build/visual_proof/hero_10s_frames/frame_0000..0299.png`), verified free of clipping, 1-SPP noise crawl, temporal ghosting, or quality downgrades (`7.29 ms p50 / 8.44 ms p95` at 1440p Showcase).
- **Interactive NVIDIA RTX Showcase Captures (`build/interactive_captures/`, `--interactive-captures`):**
  - `interactive_marbles_rtx_reblur_motion_studio.png` (live motion `NRD::REBLUR` `1 SPP` `[RBLR]` rolling marbles cascade + amber/celadon Beer-Lambert countertop caustics)
  - `interactive_marbles_rtx_basin_64spp_studio.png` & `interactive_marbles_rtx_basin_64spp_clean.png` (`64 SPP NRD::REFERENCE` crystal, celadon, amber, 24k gold, brushed nickel, and marble spheres nestled inside the porcelain vessel basin)
  - `interactive_aov_quad_inspection.png` (`2×2` diagnostic quad: `World Normal`, `Indirect Diffuse GI`, `Reflections & Refraction`, `DXR Ray Heatmap`, plus individual 1440p AOV PNGs)
  - `interactive_preset2_faucet_64spp_studio.png`, `interactive_preset3_calathea_64spp_clean.png`, `interactive_preset4_orchid_vase_64spp_clean.png`, 4-state Sun/RTXDI practical lamp sweep, `interactive_bathroom_rtx_split_wipe_studio.png`, and 12-panel `interactive_showcase_montage.png`
- **Full-Film & Telemetry Captures (`build/film_captures/`):**
  - `film_family_montage.png`, `film_midnight_montage.png`, `solos_montage.png`, `film_family_355s_presentation.png`, and `film_family_355s_stats_layout.png` (confirming `台北 / TAIPEI` + `BOO SAH SAH` clear of both `LIVE GPU` and `BENCH [B]` panels, `[F:EXPLOR]` interactive pill in Pass 4, and clean Pass-3 Spout isolation).

---

## 3. Candid 1:1 Assessment Against Cycles & NVIDIA’s *Marbles RTX*

### Where BooSahSah Closely Matches Cycles & *Marbles RTX*
- **1:1 Crop Parity Across Lighting, Metals, Dielectrics & Denoising (`dawn_1to1_crops_sheet.png`):**
  - **Architectural Chiaroscuro (`342s` & `355s`):** Extending indoor GI rays to `8.5 m` and Lambert-normalizing single-bounce window irradiance resolved the flat indoor washout; the shadowed west Venetian plaster wall, walnut cabinetry, and porcelain basin bowl now match Cycles’ tonal contrast closely, while `PSDeterministicResolve` removes 1-SPP IGN speckle without softening mosaic grout or timber grain.
  - **Crop A (Brushed Nickel & Porcelain):** Grounding `mat 23` indoor reflections to 100% ray-traced bathroom geometry eliminated outdoor IBL washout; the dark room core and crisp vertical east-window highlight along the gooseneck spout match Cycles closely.
  - **Crop B (Mirror Back Wall & Sconce):** Tightened inverse-square falloff on the reflected `2700 K` opal sconce restores Cycles’ dark taupe rear wall and framed porcelain leaf relief.
  - **Crop D & *Marbles RTX* Dielectrics:** 2-interface Snell refraction + 3D thickness Beer-Lambert absorption gives the cognac-amber bottle (`mat 35`), emerald-celadon vase (`mat 4`), and dynamic glass marbles convincing optical depth and colored shadow-ray caustics across the backsplash, marble countertop, and porcelain bowl.
  - **Live Motion & Interactive Inspection:** Flying through any scene or dropping rolling marbles runs temporal `NRD::REBLUR_DIFFUSE_SPECULAR` (`4.71–6.23 ms p50` at 1440p) before converging via `NRD::REFERENCE` up to `4096 SPP`, paired with instant 8-mode AOV buffer inspection.

### Candid Remaining Gaps Relative to Offline Cycles & *Marbles RTX*
1. **Single-Bounce Approximate GI vs. Multi-Bounce Path Tracing:** Real-time 1-SPP hemisphere rays evaluate an analytic window/sun bounce at the secondary hit rather than recursive multi-bounce path tracing. Warm color bleeding in deep double-shadowed corners (e.g., under the vanity toe-kick and on the lower `Calathea` leaf tips in Crop C, where Cycles shows warmer multi-bounce golden leaf-to-leaf scatter) remains slightly cooler/simpler than 256-SPP Cycles.
2. **Thin-Sheet Crystal Petals & Approximate Caustics:** While solid bottles, vases, droplets, and dynamic marbles trace 2-interface Snell refraction, the sculpted keepsake Glass Orchid petals in the bathroom (`Crop D`) use thin-walled refraction to avoid faceted low-thickness TIR artifacts, and dielectric caustics (`traceShadowTransmittance`) use a single-ray normal-focusing Beer-Lambert approximation rather than photon/manifold-next-event caustic transport.
3. **Exterior Mid-Rise Urban Background:** While Taipei 101 and Xinyi avenues/plazas are grounded and crisp against the Qwantani Dawn sky (`Crop E`), distant mid-rise towers still use procedural architectural window grids rather than hero photogrammetry clutter.
4. **D3D12 Debug Layer Unverified on This Host:** `d3d12SDKLayers.dll` is not installed on this machine; strict `--require-debug-layer` validation remains required on a machine with Windows Graphics Tools enabled.

---

## 4. Functional, Stress, GPU Timing, VRAM & Packaging Evidence

| Gate | Release | Debug |
|---|---|---|
| C++20 / HLSL DXC `-Ges -WX` build (`20` shader permutations incl. NRD REBLUR & Resolve) | Pass (`0` warnings) | Pass (`0` warnings) |
| Unit suite (`BooUnitTests`) | Pass (`18/18`) | Pass (`18/18`) |
| Hardware integration + live `--no-activate` swapchain suite (`REBLUR`, `REFERENCE`, 8 AOVs, *Marbles RTX*) | Pass (`18/18`) | Pass (`18/18`) |
| Cue & provenance manifest (`25` SHA-256 entries) | Pass | Pass |
| Mesh integrity & 2-run bit-identical `.bmesh` determinism | Pass (`7/7` kits, `599,394` static + `18,432` marble tris) | Same |
| Zero optional-SDK PE static-import audit (`dumpbin /dependents`, incl. `NRD`/`XInput`) | Pass | Pass |
| Smoke (`M10` Spout, 16:10 letterbox, all-fallbacks, device recovery, `M2`/`M1`, NPU Solo) | Pass | Pass |
| Trilogy native 1440p captures & determinism (`--trilogy-captures`) | Pass | — |
| Interactive RTX Showcase 1440p `REBLUR`/`NRD`/Marbles/AOV captures (`--interactive-captures`) | Pass (`12`-panel montage) | — |
| Full-Film native 1440p captures (`--film-captures`) | Pass (`72` deterministic + `2` stats PNGs) | Byte-identical (`72/72`) |
| 10-second Dawn Hero 1440p video capture (`--hero-video-capture`) | Pass (`300/300` frames) | — |
| Full-Film 224-iteration stress & recovery (`--film-stress`) | Pass (`224/224`) | Pass (`224/224`) |
| GPU retirement & Spout2 live D3D11 receiver (`--lifecycle-tests`) | Pass (`2/2`) | — |
| Single-file LZMS installer (`BooSahSah-Setup-win64.exe`, incl. `nrd\NRD.dll`) + GUI check | Pass | — |
| D3D12 debug-layer warnings/errors (`--require-debug-layer`) | Unavailable (`d3d12SDKLayers.dll` absent) | Unavailable |

### Stress Memory & Lifecycle Peaks (224 Iterations across All 7 Acts, `63` Swapchain Resizes)

| Metric (Warmup Peak `0..31` / Subsequent Peak `32..223`) | Release | Debug |
|---|---:|---:|
| DXGI Process Local VRAM | `1495.3 / 1495.3 MiB` (`0.0 MiB` growth) | `1495.3 / 1495.3 MiB` (`0.0 MiB` growth) |
| Private Bytes | `2342.1 / 2346.5 MiB` | `2331.4 / 2330.5 MiB` |
| Working Set | `848.2 / 802.2 MiB` | `765.5 / 721.3 MiB` |
| Process Handles | `1568 / 1573` | `1567 / 1572` |
| Max Adjacent 48 kHz Audio Sample Step | `0.00213893` (`< 0.010`) | `0.00213893` (`< 0.010`) |
| Non-Finite HDR Components | `0` | `0` |
| Simulated Device-Loss & Recreate Time | `3.48 s` | `4.12 s` |

### Sequential GPU Timings (`p50 / p95` ms) & Peak VRAM (`RTX 5080 Laptop GPU`, 16 GB)

Measured sequentially (`--no-activate --width 2560 --height 1440 --mute`) over 3-second playback windows (plus sustained 10-second Dawn Bathroom camera-move windows `342.0s -> 352.0s` and Interactive Explore NRD v4.18.0 windows). `Before` values are commit `3478b3c` (`286,308` untextured tris, no G-buffer/NRD/dielectric caustics); `Rebuild` values include all `617,826` loaded triangles, `42` PBR arrays, 7-MRT demodulated G-buffer, 2-interface Snell refraction + Beer-Lambert caustics, `PSDeterministicResolve`, and dual `NRD::REBLUR_DIFFUSE_SPECULAR` + `NRD::REFERENCE` pools (`docs/quality_timings.json`). Quality downgrades = **`0` on every row**.

| Sample / Window | `balanced` (1080p) `3478b3c` | `balanced` (1080p) Rebuild | `showcase` (1440p) `3478b3c` | `showcase` (1440p) Rebuild | Rebuild VRAM (`1080p / 1440p`) |
|---|---:|---:|---:|---:|---:|
| `36s` (Act I Glass Orchid) | `2.66 / 3.02` | `3.04 / 3.68` | `3.09 / 3.36` | `4.90 / 5.60` | `1123.7 / 1491.5 MiB` |
| `95s` (Act II Enamel Cathedral) | `5.06 / 5.44` | `3.10 / 3.70` | `7.09 / 8.19` | `5.00 / 5.54` | `1123.7 / 1491.5 MiB` |
| `175s` (Act III Botanical Sanctuary) | `3.58 / 3.90` | `4.31 / 5.04` | `4.23 / 4.60` | `6.68 / 7.29` | `1123.7 / 1491.5 MiB` |
| `225s` (Act IV CUDA Mutiny) | `2.76 / 3.11` | `3.17 / 3.63` | `3.58 / 3.90` | `5.33 / 6.05` | `1123.7 / 1491.5 MiB` |
| `260s` (Act V Shenzhen eVTOL) | — | `2.38 / 2.84` | — | `3.94 / 4.46` | `1142.2 / 1510.1 MiB` |
| `275s` (Act VI Combined Sanctuary) | `7.74 / 8.86` | `4.12 / 4.73` | `9.97 / 10.98` | `6.20 / 6.82` | `1123.7 / 1491.5 MiB` |
| `318s` (Act VI Grand Tutti Climax) | `7.82 / 8.93` | `4.10 / 4.70` | `11.14 / 11.77` | `6.35 / 7.05` | `1123.7 / 1491.5 MiB` |
| `342s` (Act VII Dawn Vanity Reveal) | `2.67 / 3.03` | `4.55 / 5.70` | `3.22 / 3.53` | `7.57 / 8.19` | `1123.7 / 1491.5 MiB` |
| `355s` (Act VII Dawn Penthouse Settle) | `2.91 / 3.25` | `4.48 / 5.04` | `3.63 / 3.97` | `7.43 / 8.70` | `1123.7 / 1491.5 MiB` |
| **Hero 10s Move (`342.0s -> 352.0s`)** | — | **`4.57 / 5.28`** (`551` f) | — | **`7.29 / 8.44`** (`560` f) | `1123.7 / 1491.5 MiB` |
| **Explore #1 Dawn Wide (NRD v4.18.0)** | — | **`2.91 / 3.30`** (`173` f) | — | **`4.71 / 5.20`** (`174` f) | `1124.2 / 1492.1 MiB` |
| **Explore #2 Faucet Macro (NRD v4.18.0)** | — | **`3.74 / 4.68`** (`173` f) | — | **`6.23 / 7.12`** (`175` f) | `1124.2 / 1492.1 MiB` |

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

# 2. Build & run Release + Debug smoke, unit, CTest, live swapchain, and PE zero-optional-SDK gates (always with --no-activate)
.\scripts\run_smoke.ps1 -Preset x64-base-release
.\scripts\run_smoke.ps1 -Preset x64-base-debug -SkipBenchmarks
.\build\x64-release\boo_integration_tests.exe --no-activate
.\build\x64-debug\boo_integration_tests.exe --no-activate

# 3. Capture native 1440p Trilogy, Interactive RTX Showcase (REBLUR + NRD + Marbles + AOVs), Full-Film, 10s Hero Move video, and Proof sheets
.\build\x64-release\boo_integration_tests.exe --trilogy-captures --no-activate
.\build\x64-release\boo_integration_tests.exe --interactive-captures --no-activate
.\build\x64-release\boo_integration_tests.exe --film-captures --no-activate --capture-dir build/film_captures
.\build\x64-debug\boo_integration_tests.exe --film-captures --no-activate --capture-dir build/film_captures_debug
.\build\x64-release\boo_integration_tests.exe --hero-video-capture --no-activate
ffmpeg -y -framerate 30 -i build/visual_proof/hero_10s_frames/frame_%04d.png -c:v libx264 -crf 15 -pix_fmt yuv420p build/visual_proof/dawn_hero_10s_move.mp4
python tools/build_visual_proof_sheets.py

# 4. Launch interactively in 1440p Showcase Explore Studio (press Y/M for Marbles RTX, I/F9 for 8-mode AOVs; omit --no-activate when driving interactively)
.\build\x64-release\boo_sah_sah.exe --width 2560 --height 1440 --preset showcase --explore-preset 2

# 5. Run 224-iteration Stress & Lifecycle suites
.\build\x64-release\boo_integration_tests.exe --film-stress --stress-iterations 224 --no-activate --capture-dir build/quality_stress_release
.\build\x64-debug\boo_integration_tests.exe --film-stress --stress-iterations 224 --no-activate --capture-dir build/quality_stress_debug
.\build\x64-release\boo_integration_tests.exe --lifecycle-tests --no-activate

# 6. Package & transaction-verify single-file Windows installer + GUI worker/cancel check
.\scripts\package_release.ps1
.\scripts\test_installer_gui.ps1 -SetupPath build/dist/BooSahSah-Setup-win64.exe
```
