# Graphics review: making it more cyberpunk and a real GPU workout (2026-11-20)

A read-only review by Claude Fable 5.2 of the renderer as of version 1.1.6.0. It was asked how to make the demo cooler and more cyberpunk, with more moving figures and objects, textures, detail, physics and flowing water, while keeping it easy to run on most NVIDIA cards and offering game-style Low / Medium / High / Ultra presets. All GPU costs are estimates at each preset's internal resolution, not measurements. Line numbers refer to the 1.1.6.0 working tree.

## 1. What holds back the cyberpunk feel today

- **The palette and lights are tasteful, not cyberpunk.** `emissive()` (`assets/shaders/m2_mesh.hlsl:1035`) covers six material ids: warm, ivory and teal window grids, sconces and flora. The only signage is `shopLightbox()` (:634), a five-row "TEA"/"CAFE" strip, plus the blimp. There are no holograms, media walls or flicker.
- **Most "movement" is painted in the shader.** Cars are stripes on road UVs (`avenueLighting()` :984) and pedestrians are 2D silhouettes behind glass (`shopperMask()` :562). Only a small cast moves as real geometry: the orchid, at most 18 petals, 3–5 birds (`trilogy_renderer.cpp:1560`), the courier, parcel, lifts, eVTOL, relay plane, helicopter, blimp and train.
- **Water does not flow.** Material 7 is three summed sines on the normal plus rain rings (`m2_mesh.hlsl:2084-2105`). It has no displacement, flow direction, foam, wakes or shoreline. Puddles exist only on material 1 (:2053).
- **Physics is closed-form in time.** The CUDA kernel is a function of time and seed (`live_accelerators.cpp:2005-2012`), and the fallback is `frac(r + time*k)` (`m1_taipei_slice.hlsl:4179`). Nothing collides or reacts. Rain covers only a 10×22 m box at the sanctuary. The only stateful bodies are 12 CPU marbles in Explore.
- **Fog is thin and bloom is narrow.** `rtLightShafts()` (:1838) marches 3–4 steps at a base density of 0.010–0.015, so neon has nothing to glow in. Bloom uses three mips.
- **Post-FX is already rich.** `PSCompositeCanvas` (`m1_taipei_slice.hlsl:1697`) has depth of field, chromatic aberration, an anamorphic streak, ghosts, halo, halation, vignette and grain. More lens effects will not fix the feel; density, light sources and atmosphere will.
- **Textures:** 42 layers of 1024² BC7 across three arrays (about 176 MB), one layer per material, with no detail normals, decals or grime.

## 2. Upgrades, ranked by impact per cost

| # | Upgrade | Where it plugs in | Technique | Est. ms L / M / H / U | Shows off |
|---|---|---|---|---|---|
| 1 | Neon signage and media-wall kit | `emissive()`, `shopLightbox()`, `shadeDistrictFacade()` :726; sign quads in `city_modules.cpp`; RTXDI emitters in `getRtxdiEmitter()` :1732 | Emissive glyph atlas (2–4 new layers), per-seed colour, scroll and slow flicker; vertical blade signs; LED facades sampling the live codec feed | 0.1 / 0.2 / 0.3 / 0.4 | DXR reflections, DLSS RR, NVENC/NVDEC |
| 2 | City-wide rain and wetness | `puddleMask()` :83, `clearcoatForMaterial()` :1165, particle passes | Global wetness on up-facing exterior materials; camera-following rain volume; depth-collided splashes; drips running down glass | 0.2 / 0.4 / 0.6 / 0.8 | DXR wet reflections, RR, CUDA |
| 3 | Flowing water | Material 7 :2084; `VSMain` :1898; a new compute pass | Low: flow-mapped normals. Medium: four Gerstner waves, shoreline foam, wakes behind the patrol craft and rotor wash. Ultra: a 256² FFT ocean with two cascades. A reflecting pool rippled by petals, rain and marbles; a waterfall off the terrace parapet with mist | 0.1 / 0.3 / 0.5 / 1.0 | DXR reflection and refraction, path-traced caustics |
| 4 | Real traffic, sky lanes and drone swarms | Instance loop `trilogy_renderer.cpp:1479-1575`; generalise `bird_flight.hpp` | Instanced low-poly vehicles within about 80 m, keeping shader stripes beyond; flying-car lanes between towers; 50–300 drones on closed-form splines of film time | 0.1 / 0.3 / 0.5 / 0.8 | DXR, motion vectors, RR |
| 5 | Froxel volumetric fog | Replaces `rtLightShafts()` at :2509 and :3279 | A 160×90×64 froxel compute pass lit by sun shadow, neon emitters and searchlights, temporally filtered. Low keeps analytic height fog | 0 / 0.5 / 0.8 / 1.2, but frees up to 3 shadow rays per pixel | DXR, RR |
| 6 | Crowds | New vertex-animation texture | Two or three walker meshes with baked walk cycles on the Xinyi arcades and Shenzhen quay | 0 / 0.2 / 0.4 / 0.6 | Instancing, RR |
| 7 | GPU cloth | New compute pass | XPBD banners, awnings and laundry lines driven by the existing rotor wash and courier gust. Low uses a vertex-shader sine | 0 / 0.2 / 0.3 / 0.5 | CUDA or compute |
| 8 | Reactive particles | CUDA kernel; `PSLiveParticles` | Depth collision; steam vents and neon-lit smoke; sparks from the trestle; raise the 262k cap once the CPU round trip is removed (4a) | 0.1 / 0.3 / 0.6 / 1.2 | CUDA |
| 9 | Holograms | New additive material | Translucent volume cones with scanlines and Fresnel edges; a giant orchid or koi hologram between towers driven by the Tensor field | 0.05 / 0.1 / 0.2 / 0.3 | Tensor cores, DXR |
| 10 | Detail density | `PSMain` :2017-2019; `tools/gen_pbr_textures.py` | One shared 512² detail-normal array; procedural rain leaks under ledges; seeded posters and graffiti; parallax occlusion on pavement and brick from High up | 0.1 / 0.2 / 0.4 / 0.6 | Filtering, RR |
| 11 | Neural poster atlas | `neural_rendering.hlsl` texture decoder | Decode many unique posters from small latent grids instead of shipping BC7. Prototype one first to confirm glyphs stay legible | 0 / 0.1 / 0.2 / 0.3 | Tensor cores |
| 12 | Light trails and motion blur | New pass before bloom, using the existing DLSS motion target | Per-object velocity blur; long tail-light streaks on sky lanes | 0 / 0.2 / 0.3 / 0.4 | Motion vectors |
| 13 | Post-FX finishing | Bloom mips, `bokehDepthOfField()` :1671 | Six-mip bloom, oval anamorphic bokeh, lens dirt, a teal-and-magenta night grade per act | 0.05 / 0.1 / 0.15 / 0.2 | — |
| 14 | Explore rigid debris | Marble path `trilogy_renderer.cpp:1578+` | Replace the 12 CPU marbles with 1–4k GPU rigid bodies (crates, cans, petals) | Explore only | DXR, CUDA |

Items 1, 2, 5 and 13 together deliver most of the cyberpunk look for about 1–2 ms on Medium, partly paid for by item 5 replacing the shaft rays.

## 3. Presets: Low / Medium / High / Ultra

| | Low | Medium | High | Ultra |
|---|---|---|---|---|
| Target GPU | GTX 1650/1660 (RT off), RTX 2060/3050 6 GB | RTX 3060/4060 8 GB | RTX 4070/5070 12 GB | RTX 5080/4090 16 GB |
| Frame budget | 33 ms on GTX, 16.6 ms on RTX | 16.6 ms | 16.6 ms | 16.6 ms before Frame Generation |
| Output / DLSS | 1080p; DLSS Performance on RTX, native 720p on GTX | 1080p Quality | 1440p Quality | 4K Performance or 1440p DLAA |
| DXR | Off on GTX; reflections and sun shadow on RTX | On | On | On |
| Path tracing | Off | Low | Medium | High |
| Ray Reconstruction / Frame Generation | Off / Off | On / Auto | On / Auto | On / Auto |
| Effects detail | 0 | 1 | 2 | 2 |
| Particles | 8k | 65k | 262k | 1M (after 4a) |
| Neon / signage | Emissive only | Plus RTXDI emitters | Plus media walls | Everything |
| Rain / wetness | Wet surfaces, 8k streaks | Plus splashes | Plus glass drips | Dense |
| Water | Flow normals | Gerstner, foam | Plus pool ripples | Plus FFT, waterfall mist |
| Fog | Analytic | Froxels 96×54×48 | 160×90×64 | Plus emitter shadows |
| Vehicles / drones / crowd | 20 / 30 / 0 | 60 / 100 / 40 | 150 / 200 / 120 | 300 / 300 / 250 |
| Cloth | Vertex sine | 16 banners | 48 | 96 |
| Surface detail | Detail normals | Plus decals | Plus parallax | Plus neural posters |
| Motion blur / bloom mips | Off / 4 | On / 5 | On / 6 | On / 6 |
| Neural suite | Off | Shading | Plus textures | Plus radiance cache |
| Maps from existing preset | Safe | Balanced | Showcase | Experimental4K |

**Recommended restructure.**
- Keep the internal preset ids, because the TOML and CLI parsing, diagnostics and the automatic downgrade ladder (`src/core/health.cpp:342-360`) depend on them.
- Relabel them Low / Medium / High / Ultra in the UI.
- Make each preset a bundle of defaults (DLSS mode, path tracing, effects detail and per-feature levels as new JSON fields), with resolution following the display.
- Show "Custom" when any setting is overridden.
- Let Low use DXR on RTX cards, so an RTX 2060 still shows reflections.
- Audit whether `rt_bounces` and `particle_budget` are fully consumed before adding fields.

**Headroom.** The 5080 Laptop measured a 13.86 ms median at 1440p with Quality RR. High at 1440p therefore has only about 2.5 ms spare, so the section 4 savings are a prerequisite for the full High column.

## 4. Optimisations that free budget

- **a. CUDA round-trips through the CPU every frame.** Each frame the kernel launches, copies to the host, calls `cuStreamSynchronize` on the render thread, then memcpys into an upload buffer and copies again (`live_accelerators.cpp:2021-2038`, `m1_taipei_slice.cpp:3509-3523`). The tensor field does the same. Fix it by sharing a D3D12 heap with CUDA or porting the kernels to D3D12 compute. That removes a blocking sync and makes 1M particles feasible.
- **b. The TLAS is fully rebuilt every pass, static city included** (`trilogy_renderer.cpp:1638`). Refit when the instance set is unchanged, or merge static modules per district. Measure with `stats.rayInstances` first.
- **c. Meshes are unwelded.** Every asset has vertices = 3 × triangles, drawn as non-indexed vertex pulls. Welding would cut vertex work, shrink BLAS memory, and reduce the roughly 92 MB of `.bmesh`.
- **d. Penumbra probes may be redundant with RR on.** All ray work runs full-rate in the forward pixel shader, and the extra probes (:2367-2387) are probably unnecessary when RR denoises a 1-spp image. Confirm with an A/B capture.
- **e. Batching only merges adjacent instances of the same mesh** (:1447-1453). Sort city emits by mesh id.
- **f. The pixel shader writes a UAV counter** (:2513-2519). Confirm early-Z is still forced after the prepass.
- **g. No steady-state blocking readbacks were found.**

## 5. Risks

- **Determinism.** The release test reproduces identical pixels after seeks. Closed-form effects (Gerstner/FFT water, spline traffic, drones, animated crowds) are safe. Stateful simulations (pool ripples, cloth, rigid bodies, fog history) need fixed-tick re-simulation from the shot start on seek, or must stay in Explore with a closed-form stand-in for the film.
- **Photosensitivity.** Neon flicker, hologram glitches, sparks and lightning must respect the reduced-flash setting: flicker under 3 Hz and no saturated red strobing, covered by the reduced-flash test.
- **Localisation.** New sign text must follow the locale-aware blimp signs (`cityBlimpSigns(languageTag)`) across 25 locales, or use abstract glyphs.
- **Store size.** Each new 1024² BC7 layer across three arrays adds about 4.2 MB raw. Prefer procedural content, 512² detail arrays and neural posters. Mesh welding recovers some space.
- **VRAM.** Safe's soft limit is 4 GB. Froxels, FFT water and cloth add under 100 MB; Low on 6 GB cards should skip the neural suite and froxels.
- **Mac port.** Keep new compute passes backend-neutral for the M6 port.
