# BooSahSah Visual Rebuild, PBR/DXR Pipeline, and Verification Report

Verified locally on 2026-11-12 on branch `codex/film-quality-robustness` (starting from commit `3478b3c`).
- **Host:** Windows 11 (Build 26200), Intel Core Ultra 9 275HX (63 GB RAM), **NVIDIA GeForce RTX 5080 Laptop GPU** (16 GB VRAM, Driver 621.14, Feature Level 12_2, Shader Model 6.7, DXR Tier 1.2, CUDA Driver API 13.5), Intel AI Boost NPU, Blender 5.2.1 LTS, MSVC C++20 (`/W4`, DXC `-Ges -WX -O3`).
- **Baseline Preservation:** Pre-rebuild captures from `3478b3c` are preserved under `build/before_visual_rebuild/`, and timing receipts from `3478b3c` are preserved under `build/quality_after/timings/`.
- **D3D12 Debug Layer Availability:** `C:\Windows\System32\d3d12SDKLayers.dll` (Windows Graphics Tools) is **absent on this host**. All receipts honestly record `"debug_layer_active": false`, `null` error/warning counts, and `"warning_validation": "unavailable: Windows Graphics Tools debug layer is absent"`. Running `--require-debug-layer` rejects this host as expected (`build/quality_strict_debug_probe.log`). No clean D3D12 debug-layer run is claimed.

---

## 1. Asset & PBR Shading Architecture (Marbles RTX Craftsmanship Reference)

Guided by NVIDIA’s *Marbles RTX* asset craftsmanship ([Substance 3D Magazine](https://magazine.substance3d.com/how-nvidias-marbles-rtx-demo-was-textured-with-substance/))—metric real-world texel density, bevelled silhouette edges with face-weighted normals, physically grounded roughness/micro-imperfection breakup, and unified ray-hit shading—while preserving BooSahSah’s Taiwanese Glass Orchid and oral-care narrative:

1. **Runtime Texture Pipeline (`BTEX001`, `include/boo/btex.hpp`, `src/render/btex.cpp`, `tools/gen_pbr_textures.py`):**
   - **42 PBR Material Array Slices (`1024×1024`, 11 mip levels, 16× anisotropic filtering):**
     - `assets/textures/pbr_albedo_alpha.btex` (`DXGI_FORMAT_BC7_UNORM_SRGB`): sRGB base color + botanical cutout opacity mask.
     - `assets/textures/pbr_normal.btex` (`DXGI_FORMAT_BC5_UNORM`): linear tangent-space normal XY with orthonormal MikkTSpace-equivalent tangent/bitangent handedness reconstruction (`tangent.w` sign validated against UV gradient orientation).
     - `assets/textures/pbr_ormt.btex` (`DXGI_FORMAT_BC7_UNORM`): linear Ambient Occlusion (`R`), Toksvig mip-compensated Roughness (`G`, eliminating distant specular shimmer on grooved/brushed metals and tiles), Metallic (`B`), and Translucency / Clearcoat (`A`).
     - `assets/textures/pbr_ibl_env.btex` (`DXGI_FORMAT_R16G16B16A16_FLOAT`): GGX-prefiltered HDR environment arrays (misty Taipei Basin dawn sky + warm architectural interior bounce).
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
3. **D3D12 / DXR 1.1 Renderer Upgrades (`src/render/trilogy_renderer.cpp`, `assets/shaders/m2_mesh.hlsl`):**
   - **Hardware Depth Prepass (`m2_depth_cutout_ps.cso` + `DepthFunc = EQUAL`):** Eliminates all back-to-front multi-kit and multi-layer foliage DXR overdraw (`84.8M -> 19.4M` rays/frame at 1440p `355s`).
   - **Consistent Direct & Ray-Hit Material Evaluation:** Secondary reflection/transmission rays fetch full `BC7`/`BC5` PBR textures at ray-cone mip LODs and evaluate GGX + IBL + sun lighting consistently with primary hits.
   - **Alpha-Cutout Foliage with Matching Ray-Query Visibility:** `mat 29` (`Calathea`) and `mat 36` (`Fern`) use `D3D12_RAYTRACING_GEOMETRY_FLAG_NONE` BLASes with matching `a >= 0.44` candidate commit logic across primary visibility, soft sun shadow rays (`traceShadow`), mirror rays (`traceMirrorScene`), and sharp reflection/refraction cones (`coneSpread < 0.045`), while diffuse GI/AO and rough cones use hardware `RAY_FLAG_FORCE_OPAQUE` traversal.
   - **Motivated Dawn Illumination, Soft Shadows, Hemisphere GI & Mirror-to-Glass Transmission:** Stratified 2-to-6 sample Vogel-disk soft sun shadows, 4-ray cosine-weighted DXR hemisphere GI + multi-scale contact AO (`4 mm` crevices to `3.85 m` window portal), AR-coated architectural float glass (`mat 40`), and `traceMirrorScene` allowing the flat vanity mirror (`mat 34`) to see cleanly through the celadon-tinted glass shower enclosure (`mat 4`) to the mosaic shower wall and rain showerhead behind the camera.

---

## 2. Visual Proof, Cycles Reference Comparison & 10s Hero Camera Move

All visual proof artifacts are generated at native **2560×1440** with `--no-activate`:
- **Blender 5.2 Cycles Path-Traced Reference Renders (`tools/render_cycles_reference.py`):**
  - `build/visual_proof/dawn_cycles_342s.png` and `build/visual_proof/dawn_cycles_355s.png` (rendered from the exact `.bmesh` geometry, PBR `.btex` source maps, sun angle, and timeline camera states).
- **Before (`3478b3c`) vs. Cycles Reference vs. Real-Time D3D12/DXR Proof Sheets (`tools/build_visual_proof_sheets.py`):**
  - `build/visual_proof/dawn_before_cycles_after_comparison.png` (3-way 1440p Dawn Bathroom comparison at `342.0s` and `355.0s`)
  - `build/visual_proof/dawn_1to1_crops_sheet.png` (5 unscaled 1:1 native pixel crops comparing D3D12/DXR vs. Cycles across the porcelain basin undercut/CNC faucet/3D water droplets, flat mirror reflection, Calathea leaf venation/translucency, waffle towels/amber soap/toothbrush bristles/keepsake orchid, and AR float window/Taipei 101 vista)
  - `build/visual_proof/full_film_before_after_sheet.png` (Before vs. After across Acts 1, 2, 3, 5, and 7)
- **Recorded 10-Second Hero Camera Move (`342.0s -> 352.0s`, 300 frames @ 30 fps, native 2560×1440):**
  - `build/visual_proof/dawn_hero_10s_move.mp4` (and raw PNG sequence `build/visual_proof/hero_10s_frames/frame_0000..0299.png`), verified free of clipping, temporal popping, shadow breakup, or quality downgrades.
- **Full-Film & Telemetry Captures:**
  - `build/film_captures/` (`film_family_montage.png`, `film_midnight_montage.png`, `solos_montage.png`, `film_family_355s_presentation.png`, `film_family_355s_stats_layout.png` confirming `台北 / TAIPEI` + `BOO SAH SAH` clear of both `LIVE GPU` and `BENCH [B]` panels, and clean Pass-3 Spout isolation).

---

## 3. Candid Assessment Against NVIDIA’s *Marbles RTX* Reference & Remaining Weaknesses

### Where BooSahSah Approaches the *Marbles RTX* Standard
- **Close-Up Hero Materiality & Bevelled Silhouettes:** In the Dawn Penthouse Bathroom (`342s–360s`), Enamel Cathedral (`95s`), and Botanical Sanctuary (`175s`), close-up props survive 1:1 native 1440p crop inspection: the smooth double-walled ceramic vessel basin with recessed chrome overflow/drain, CNC brushed-nickel faucet with knurled collar and 3D water droplets, directional smoked-walnut cabinetry grain, fluted finger-mosaic tiles with individual grout normal crevices, scanned `Calathea orbifolia` leaves with herringbone venation and backlit subsurface glow, folded waffle-weave washcloths, amber glass apothecary bottle, and optical mirror reflections closely parallel the Blender 5.2 Cycles path-traced reference.
- **Coherent Motion & Specular Stability:** Toksvig roughness mip-mapping, 16× anisotropic filtering, deterministic IGN-stratified soft shadows/GI, and the hardware Z-prepass keep reflections, fine grout lines, brushed metals, and leaf shadows temporally coherent throughout the 10-second camera pullback without stochastic denoiser ghosting or lag.

### Candid Remaining Gaps Relative to *Marbles RTX*
1. **Single-Bounce Hemisphere GI + IBL vs. Multi-Bounce Path Tracing:** *Marbles RTX* uses full multi-bounce path tracing with ReSTIR/NRD denoising. BooSahSah uses deterministic 4-ray DXR hemisphere GI blended (`84% / 16%`) with GGX-prefiltered IBL (`pbr_ibl_env.btex`). As visible in `dawn_before_cycles_after_comparison.png` (`355s`), Cycles produces deeper multi-bounce shadow contrast on the left Venetian plaster wall and warmer multi-bounce wood-cavity darkening under the vanity drawers than our real-time single-bounce + IBL approximation.
2. **Thin-Walled Single-Hit Glass vs. Multi-Bounce Dielectric Refraction & Caustics:** Glass materials (`mat 3, 4, 18, 35, 40`) trace one reflection ray and one refracted transmission ray with Beer-Lambert absorption and analytic dispersion rather than multi-bounce internal total reflection or refractive caustics. Consequently, the amber soap bottle (`mat 35`) and keepsake Glass Orchid vase (`mat 4`) appear slightly brighter and less internally dense than in the 6-bounce Cycles reference (`dawn_1to1_crops_sheet.png`, Crop D).
3. **Exterior Urban Background Detail vs. Interior Hero Props:** While Taipei 101 and Xinyi District are now grounded on avenues, podiums, streetlamps, and basin mountain ridges, the mid-rise background towers still rely on procedural architectural facade grids rather than hero-grade photogrammetry/kitbashed street clutter.
4. **Combined Grand-Tutti 1440p Frame Time (`275s` / `318s`):** At `balanced` (1080p default), every scene across the 360s film runs at `1.58–14.54 ms` (`0` quality downgrades). At `showcase` (native 1440p), the Dawn Bathroom hero move (`9.21 ms p50`) and solo/act shots (`1.98–12.62 ms p50`) stay comfortably inside `16.67 ms`, whereas the simultaneous 4-kit + 65K CUDA + 64-layer Tensor grand-tutti climax at `275s` (`17.15 ms p50`) and `318s` (`18.69 ms p50`) runs at ~54–58 fps (`1` quality-governor step at the end of a sustained `318s` window).
5. **Raster Fallback Mode (`--disable-rt`):** Uses identical PBR meshes, normal maps, textures, and prefiltered IBL, substituting environment lookups where DXR reflections, refractions, and ray-traced shadows are disabled.
6. **D3D12 Debug Layer Unverified on This Host:** `d3d12SDKLayers.dll` is not installed on this machine; strict `--require-debug-layer` validation remains required on a machine with Windows Graphics Tools enabled.

---

## 4. Functional, Stress, GPU Timing, VRAM & Packaging Evidence

| Gate | Release | Debug |
|---|---|---|
| C++20 / HLSL DXC `-Ges -WX` build | Pass (`0` warnings) | Pass (`0` warnings) |
| Unit suite (`BooUnitTests`) | Pass (`18/18`) | Pass (`18/18`) |
| Hardware integration suite (`BooHardwareIntegrationTests`) | Pass (`17/17`) | Pass (`17/17`) |
| Cue & provenance manifest (`25` SHA-256 entries) | Pass | Pass |
| Mesh integrity & 2-run bit-identical `.bmesh` determinism | Pass (`7/7` kits, `599,394` tris) | Same |
| Zero optional-SDK PE static-import audit (`dumpbin /dependents`) | Pass | Pass |
| Smoke (`M10` Spout, 16:10 letterbox, all-fallbacks, device recovery, `M2`/`M1`, NPU Solo) | Pass | Pass |
| Trilogy native 1440p captures & determinism (`--trilogy-captures`) | Pass | — |
| Full-Film native 1440p captures (`--film-captures`) | Pass (`72` deterministic + `2` stats PNGs) | Byte-identical (`72/72`) |
| 10-second Dawn Hero 1440p video capture (`--hero-video-capture`) | Pass (`300/300` frames) | — |
| Full-Film 224-iteration stress & recovery (`--film-stress`) | Pass (`224/224`) | Pass (`224/224`) |
| GPU retirement & Spout2 live D3D11 receiver (`--lifecycle-tests`) | Pass (`2/2`) | — |
| Single-file LZMS installer (`BooSahSah-Setup-win64.exe`) + GUI responsiveness/cancel | Pass | — |
| D3D12 debug-layer warnings/errors (`--require-debug-layer`) | Unavailable (`d3d12SDKLayers.dll` absent) | Unavailable |

### Stress Memory & Lifecycle Peaks (224 Iterations across All 7 Acts)

| Metric (Warmup Peak `0..31` / Subsequent Peak `32..223`) | Release | Debug |
|---|---:|---:|
| DXGI Process Local VRAM | `633.4 / 633.4 MiB` (`0.0 MiB` growth) | `633.4 / 633.4 MiB` (`0.0 MiB` growth) |
| Private Bytes | `1491.4 / 1533.6 MiB` | `1598.4 / 1645.0 MiB` |
| Working Set | `845.9 / 875.6 MiB` | `871.8 / 876.5 MiB` |
| Process Handles | `1573 / 1575` | `1569 / 1571` |
| Max Adjacent 48 kHz Audio Sample Step | `0.00213893` (`< 0.010`) | `0.00213893` (`< 0.010`) |
| Simulated Device-Loss & Recreate Time | `3.16 s` | `3.93 s` |

### Sequential GPU Timings (`p50 / p95` ms) & Peak VRAM (`RTX 5080 Laptop GPU`)

Measured sequentially (`--no-activate --width 2560 --height 1440 --mute`) over 3-second playback windows (plus sustained 10-second Dawn Bathroom camera-move windows `342.0s -> 352.0s`). `Before` values are commit `3478b3c` (`286,308` untextured tris); `After` values are the full `599,394`-tri, `42`-material PBR + DXR GI/soft-shadow rebuild (`docs/quality_timings.json`).

| Sample / Window | `balanced` (1080p) `3478b3c` | `balanced` (1080p) Rebuild | `showcase` (1440p) `3478b3c` | `showcase` (1440p) Rebuild | Rebuild VRAM (`1080p / 1440p`) |
|---|---:|---:|---:|---:|---:|
| `36s` (Act I Glass Orchid) | `2.66 / 3.02` | `4.09 / 4.38` | `3.09 / 3.36` | `4.77 / 5.13` | `586.2 / 629.7 MiB` |
| `95s` (Act II Enamel Cathedral) | `5.06 / 5.44` | `6.72 / 7.10` | `7.09 / 8.19` | `8.08 / 8.46` | `586.2 / 629.7 MiB` |
| `175s` (Act III Botanical Sanctuary) | `3.58 / 3.90` | `10.56 / 11.07` | `4.23 / 4.60` | `12.62 / 13.39` | `586.2 / 629.7 MiB` |
| `225s` (Act IV CUDA Mutiny) | `2.76 / 3.11` | `4.38 / 4.63` | `3.58 / 3.90` | `5.05 / 5.36` | `586.2 / 629.7 MiB` |
| `260s` (Act V Shenzhen eVTOL) | — | `1.58 / 1.79` | — | `1.98 / 2.20` | `604.7 / 648.2 MiB` |
| `275s` (Act VI Combined Sanctuary) | `7.74 / 8.86` | `12.91 / 13.34` | `9.97 / 10.98` | `17.15 / 17.81` | `586.7 / 629.7 MiB` |
| `318s` (Act VI Grand Tutti Climax) | `7.82 / 8.93` | `14.54 / 14.97` | `11.14 / 11.77` | `18.69 / 19.20` | `586.2 / 629.7 MiB` |
| `342s` (Act VII Dawn Vanity Reveal) | `2.67 / 3.03` | `8.71 / 9.18` | `3.22 / 3.53` | `11.92 / 12.66` | `586.2 / 629.7 MiB` |
| `355s` (Act VII Dawn Penthouse Settle) | `2.91 / 3.25` | `6.81 / 7.12` | `3.63 / 3.97` | `8.65 / 9.05` | `586.2 / 629.7 MiB` |
| **Hero 10s Move (`342.0s -> 352.0s`)** | — | **`6.93 / 8.44`** (`563` f) | — | **`9.21 / 11.14`** (`559` f) | `586.2 / 629.7 MiB` |

---

## 5. Reproduction Instructions

```powershell
# 1. Regenerate PBR .btex texture arrays, deterministic .bmesh kits, Cycles references, and SHA-256 manifest
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python tools/gen_pbr_textures.py
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python tools/gen_trilogy_meshes.py
& "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --factory-startup --python tools/render_cycles_reference.py
.\.venv\Scripts\python.exe tools/verify_trilogy_meshes.py
.\.venv\Scripts\python.exe tools/refresh_asset_hashes.py

# 2. Build & run Release + Debug smoke, unit, CTest, and PE zero-optional-SDK gates (always with --no-activate)
.\scripts\run_smoke.ps1 -Preset x64-base-release
.\scripts\run_smoke.ps1 -Preset x64-base-debug -SkipBenchmarks

# 3. Capture native 1440p Trilogy, Full-Film, 10s Dawn Hero Move video, and Visual Proof comparison/crop sheets
.\build\x64-release\boo_integration_tests.exe --trilogy-captures --no-activate
.\build\x64-release\boo_integration_tests.exe --film-captures --no-activate --capture-dir build/film_captures
.\build\x64-debug\boo_integration_tests.exe --film-captures --no-activate --capture-dir build/film_captures_debug
.\build\x64-release\boo_integration_tests.exe --hero-video-capture --no-activate --capture-dir build/visual_proof
ffmpeg -y -framerate 30 -i build/visual_proof/hero_10s_frames/frame_%04d.png -c:v libx264 -crf 15 -pix_fmt yuv420p build/visual_proof/dawn_hero_10s_move.mp4
python tools/build_visual_proof_sheets.py

# 4. Run 224-iteration Stress & Lifecycle suites
.\build\x64-release\boo_integration_tests.exe --film-stress --no-activate --capture-dir build/quality_stress_release
.\build\x64-debug\boo_integration_tests.exe --film-stress --no-activate --capture-dir build/quality_stress_debug
.\build\x64-release\boo_integration_tests.exe --lifecycle-tests --no-activate

# 5. Package & transaction-verify single-file Windows installer + GUI worker/cancel check
.\scripts\package_release.ps1
.\scripts\test_installer_gui.ps1 -SetupPath build/dist/BooSahSah-Setup-win64.exe
```
