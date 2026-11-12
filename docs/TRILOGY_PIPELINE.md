# M2 trilogy implementation and acceptance receipt

The default cue is `assets/cues/m2_trilogy_180s.json`: 8,640,000 samples at 48 kHz across three 60-second acts (Shanghai `0–60s`, Shenzhen `60–120s`, Taipei `120–180s`). Explicit M1 (`assets/cues/m1_taipei_60s.json`) and M0 (`assets/cues/film_360s.json`) cue paths remain supported. `G` seeks to 163 seconds for M2 and 43 seconds for M1; keys 1–6 retain the established full/RT/Tensor/NPU/CUDA/finale transport mapping. `--start-seconds` adds deterministic startup seeking for capture and timing runs. All automated windowed runs use `--no-activate`.

## Asset production

Generate with `"C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --background --python tools/gen_trilogy_meshes.py`, then run `python tools/verify_trilogy_meshes.py`. Blender 5.2.1 LTS is the installed executable. No Blender or asset-import SDK is linked into the runtime. All modeled geometry is original procedural work, declared CC0 in `assets/manifest.json`.

The exporter applies evaluated Blender modifiers, triangulates explicitly, computes fixed-order smooth normals (and intrinsic surface-derivative normals on closed botanical shells), exports UV-derived orthogonal tangents plus per-vertex thickness/roughness attributes, canonicalizes triangle order, rejects collapsed triangles, and writes files atomically. Each city kit is partitioned spatially across `part = 0..5` submeshes so DXR bottom-level acceleration structures maintain tight bounding volumes rather than scene-wide overlapping AABBs. The exporter checks manifold edges before triangle packing; the CPU verifier checks nondegenerate exported triangles, unit normals, orthonormal tangents, tight bounds, CRC32, and manifest/report SHA256.

BMESH001 version 1 is little-endian: a 64-byte header (8-byte magic, version, header size, vertex stride, vertex/index/submesh counts, six float32 AABB coordinates, payload CRC32, payload size); 24-byte submesh records (first index, index count, material, part, first vertex, vertex count); 40-byte vertices (float32 position3, snorm16 normal4, snorm16 tangent4 including handedness, float16 UV2/thickness/roughness, uint16 material/part); uint32 triangle indices. Runtime vertices expand to 64 bytes for shader structured-buffer alignment. Loader count/size/range/integrity checks complete before replacing the output mesh.

Missing or corrupt meshes select deterministic low-detail fallback geometry in `src/render/m1_scene.cpp`. `--inject-mesh-fail` exercises that route. Missing mesh manifest entries produce a fallback warning instead of aborting startup; other missing registered assets remain errors.

## Visual-quality revision summary

1. **Act 3 — Taipei (`120–180s`, captures `168s` and `176s`)**:
   - Removed the procedural `k == 4` column/slab kit from `TrilogyMeshRenderer::initialize` that previously occluded the finale foreground.
   - Authored a grounded Xinyi urban basin (`taipei_101_hero.bmesh`): foreground Sanctuary Rooftop Rain-Terrace with granite parapet, recessed warm edge grazers, reflecting pool, bronze plinth collar, skybridge podiums, varied Xinyi high-rises, and distant misty Taipei Basin ridges.
   - Modeled Taipei 101 at authored 508 m scale (`world scale = 0.080` at `(18.5, 0.0, 60.0)`) with an open-framed 88F–91F observatory gallery, bronze corner columns, 4-stanchion bumper cradle, and true-scale 5.5 m (`r = 2.75 m`) 41-plate golden Tuned Mass Damper sphere. Act 3's camera opens with a close aerial observatory pass (`120–141s`) where the damper's stacked rings and cradle are directly legible at true scale, descends onto the Sanctuary Terrace for the `168s` bloom close-up, and settles into an unobstructed `176s` wide composition pairing the left-foreground Glass Orchid with the complete Taipei 101 silhouette on the right.
2. **Botanical Glass Orchid (`glass_orchid_hero.bmesh`, `168s` and `176s`)**:
   - Sculpted watertight two-sided botanical shells (`closed_blade` and `closed_tube`) differentiating 3 narrower keeled pale-celadon sepals (`mat = 4`), 2 broad clawed rhomboid-obovate low-iron crystal lateral petals (`mat = 3`) with basal negative-space notches and gentle flutes, and a 3-lobed ruby-amber Liuli crystal labellum (`mat = 9`) with erect lateral chalice lobes, hastate mid-lobe, upturned apical cirri, bilobed 24k gold callus (`mat = 8`), arched column/porcelain anther cap, S-curved stem, clasping bracts, and 3 pointed crystal buds.
   - Separated thin glass (`InstanceMask = 0x02`) from opaque scene geometry (`InstanceMask = 0x01`) in DXR so reflection and refraction rays sample the surrounding Xinyi skyline without hollow-shell back-wall self-intersection artifacts.
3. **Act 2 — Shenzhen (`60–120s`, captures `75s` and `108s`)**:
   - Replaced the isolated floating ring and repeated box towers with a connected Shenzhen Bay / Houhai waterfront canal, south civic vertiport peninsula (arterial boulevards, median planting, stepped stone amphitheater terraces, glazed passenger terminal, cantilevered `H` Hero Pad + West Pad), tied-arch canal bridge, north boardwalk, retail podiums with roof gardens, interlocking twin skybridge Tech HQ towers, tapered supertall with lantern crown, and varied setback urban masses (`shenzhen_evtol.bmesh`).
   - Modeled a winged 6-tilt-rotor eVTOL with smoked cockpit canopy (`mat = 18`) and 3-blade carbon-titanium rotors (`R_x(tilt) * R_y(spin)` around each nacelle pivot) transitioning from `49°` forward-tilted golden-hour bay cruise (`75s`) to upright nocturnal VTOL hover over the Hero Pad (`108s`).
   - Added daylight curtain-wall spandrel/mullion articulation and dusk-gated window/urban illumination (`nightFactor = smoothstep(0.68, 0.08, daylight)`) so `75s` -> `108s` transforms sky, sunlight, interior rooms, terminal/pad lighting, and multi-wave canal reflections rather than merely darkening the frame.
4. **Act 1 — Shanghai (`0–60s`, captures `15s` and `45s`)**:
   - Split the crossing into a curved-chord riveted steel Waibaidu-style through-truss approach span (`z = -54..+6`, with gusset plates, X-wind-bracing, walkways, and structurally mounted railway signal gantry) transitioning past the `z = +6` North Portal onto an open low-parapet plate-girder river span (`z = +6..+66`) with trackside third-rail conductor so no overhead wires cut across the skyline reveal (`shanghai_trestle.bmesh`).
   - Grounded both banks with West Bund heritage masonry blocks, two middle-distance Huangpu River cargo barges, and a Pudong granite seawall/promenade supporting Oriental Pearl (tripod struts, 3 columns, spheres, observation rings, antenna), stepped Jin Mao Tower, twisted Shanghai Tower, chamfered SWFC aperture, and varied Lujiazui towers.
   -Choreographed an off-axis low rail-level perspective at `15s` (four gleaming running/guard rails and localized wet deck plates between matte sleepers) rising smoothly out through the portal into an unobstructed river-and-skyline reveal at `45s`, with sparse directional rain that keeps bridge geometry crisp.

## Rendering and hardware evidence

`TrilogyMeshRenderer` feeds the same triangles and per-part transforms to SM 6.0 rasterization and DXR BLAS/TLAS. Raster silhouettes therefore match RT silhouettes. Inline queries provide reflections, short-range contact occlusion (`RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH`), and thin-shell refraction. M2 owns depth and mesh resources while retaining the established M1 particles, Tensor field, bloom/composite, canonical `RGB10A2` `CanvasClean`, isolated `canvasAbCompare`, codec round trip, and HUD.

The M2 shader counts actual issued ray queries using wave reductions into a GPU counter. A separate sampled intersection counter verifies live work; a standalone benchmark cannot promote live status. RT timing covers the scene pass, including TLAS rebuild and primary shading, rather than pretending to isolate traversal alone. CUDA and Tensor retain verified device work; their outputs influence rain/downwash/sanctuary motes and sky/surface irradiance. Codec remains explicitly optional/standby until selected. M2 captures label the independently measured benchmark receipt in `BENCH`; headless FPS is zero rather than a fabricated presentation rate.

The 180-second synth has a separate duration-gated branch: rail pulse/minor ninths, warm major aerial movement, then sanctuary/bloom/finale. The 60-second synthesis arithmetic is retained. WASAPI presentation tracking, migration observer, 768-sample seek crossfade, and 576-sample gain ramps remain in place.

## Candid visual weaknesses and scope boundaries

- **Stylized procedural geometry, not survey-accurate CAD**: Landmarks (Oriental Pearl, Jin Mao, Shanghai Tower, SWFC, Houhai supertall, Taipei 101) and urban blocks are stylized low-to-mid-poly procedural interpretations rather than survey-verified architectural models, photogrammetry, or botanical CT scans. Surrounding mid-rise/high-rise blocks still rely on procedural curtain-wall UV/world-space mullion and room-occupancy patterns rather than bespoke per-building facade textures, street furniture, or vehicular traffic.
- **Single-bounce thin-shell glass & water shading**: The DXR path traces one reflection ray and one refracted transmission ray per glass surface against opaque geometry (`mask = 0x01`) rather than multi-bounce dielectric path tracing, internal total reflection, or spectral caustics. Because `TrilogyMeshRenderer` uses an opaque depth-buffered pass without order-independent transparency, overlapping glass organs do not transmit through multiple stacked glass layers behind the frontmost glass surface.
- **Discrete rotor blades without temporal motion blur**: The eVTOL uses 3 pitched aerofoil blades per hub without temporal accumulation or alpha-blended rotor blur discs (which previously looked like opaque plastic paddles in depth-buffered raster/DXR); in still captures they read as high-shutter-speed frozen blades rather than long-exposure motion blur.
- **Scale tradeoff on Taipei 101's damper**: Keeping the Tuned Mass Damper at honest 5.5 m diameter inside the 508 m tower makes it clearly detailed in the `120–141s` close aerial pass, while reducing it to a small warm-gold accent inside the upper observatory aperture in the wide `176s` finale. Reference: [Taipei 101 official damper description](https://www.taipei-101.com.tw/tw/observatory/feature/damper).
- **Host validation & thermal scope**: GPU timings measure 2-second windows on this RTX 5080 Laptop GPU rather than a sustained thermal soak. Debug binaries were built and smoke-tested, but Windows Graphics Tools (`d3d12SDKLayers.dll`) is absent on this machine; `--d3d-debug` logs a warning and cannot provide D3D12 debug-layer validation.

## Verified results — 2026-11-11

Both requested smoke commands (`scripts/run_smoke.ps1 -Preset x64-base-release` and `scripts/run_smoke.ps1 -Preset x64-base-debug -SkipBenchmarks`) completed with exit code `0` after all code and asset edits. Each configuration passed all four CTest entries: 13/13 unit cases, 11/11 hardware integration cases (`--no-activate`), cue/manifest validation, and the M2 native capture suite. PE import audits confirmed zero forbidden SDK/import libraries. Explicit M1 and M0 backward-compatibility launches succeeded. Expected injected-shader-failure warnings and the unavailable Graphics Tools warning remain; no compiler warnings/errors or runtime effect faults occurred.

M2 live swapchain tests (`M2LiveNoActivateSwapchainDeterminism`) exercised all six capture samples through direct seek, preceding playback, backward step, HUD/A-B cycling, accelerator toggle restoration, and Showcase -> Safe -> Showcase restoration with exact `RGB10A2` `CanvasClean` determinism and foreground-window preservation (`--no-activate`). Score seek/wrap maximum adjacent stereo step was **0.00212065** (`< 0.010`). Camera positions and targets remained C0-continuous across both act boundaries.

Two independent Blender 5.2.1 LTS runs produced byte-identical `.bmesh` outputs across all four kits (`202,120` total triangles, `0` nonmanifold edges), verified by `tools/verify_trilogy_meshes.py`:

- `shanghai_trestle`: 42,396 triangles (36 submeshes); SHA256 `d95885c632ada24ac07cfc5e42b608ff7841d73adc670ef296d21b855aa2bda8`.
- `shenzhen_evtol`: 37,646 triangles (75 submeshes); SHA256 `cd29dd429c4161bc33d30f8ceeb9ab965c2ce0665333a2249b24d6f4f7953891`.
- `taipei_101_hero`: 87,922 triangles (35 submeshes); SHA256 `117572047b6f22c7bc905bf192945e8a986f8e38ad791b7c2972b943a9755831`.
- `glass_orchid_hero`: 34,156 triangles (11 submeshes); SHA256 `5c98c7077ab39399e4015b59b504eea8f22af2980a8dcae2d9154f8f1238eb7e`.

GPU timestamp measurements on the NVIDIA GeForce RTX 5080 Laptop GPU, Release, native canvas, two-second moving windows beginning at each listed sample (`--no-activate --mute --preset <preset> --start-seconds <s> --duration-seconds 2`, Codec in normal standby, `0` quality downgrades, `0` effect faults). Values are **median (`p50`) / `p95` milliseconds**:

| Start (s) | Native 2560×1440 (`showcase`) | Native 3840×2160 (`4k_experimental`) |
|---:|---:|---:|
| 15 | 5.927 / 6.683 | 12.742 / 13.299 |
| 45 | 4.580 / 5.073 | 8.988 / 10.300 |
| 75 | 4.648 / 5.252 | 9.172 / 10.632 |
| 108 | 5.505 / 6.399 | 11.757 / 12.732 |
| 168 | 6.299 / 6.450 | 12.652 / 12.832 |
| 176 | 4.278 / 4.446 | 7.796 / 8.160 |

Evidence: [scene contact sheet](../build/trilogy_montage.png), [HUD and A/B contact sheet](../build/trilogy_ab_montage.png), [native captures](../build/trilogy_captures), [Release smoke log](../build/trilogy_smoke_release.log), [Debug smoke log](../build/trilogy_smoke_debug.log), [mesh verifier](../build/trilogy_mesh_verification.log), [reproducibility receipt](../build/trilogy_reproducibility.json), and [timing summary](../build/trilogy_measurements.json).

Recovery snapshots taken before edits: `D:\boo-sah-sah-backups\trilogy-20261111-104051` (initial M2 snapshot) and `D:\boo-sah-sah-backups\visual-revision-20261111-123143` (pre-visual-revision baseline code, assets, and captures).
