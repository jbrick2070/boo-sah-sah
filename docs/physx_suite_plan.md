# PhysX suite plan

Draft, 2026-11-20. A plan only: nothing beyond the smoke bridge is built yet.

The goal is to use every capability of NVIDIA's PhysX SDK, one at a time and under control, in a way that belongs to the film.

## Two modes

- **Film (baked).** The film stays deterministic: the same frame at the same film time, every run. Where physics serves a shot, it is simulated offline with the real SDK, recorded against film time and shipped as data. The determinism tests and seeking keep working, and the Store build needs no live solver for the story.
- **Live (dial).** A **PhysX** row on the dashboard and `Shift+P` dial through the genres below, one at a time. Each runs live on the real SDK and shows its own figures: step time on the CPU and the GPU, bodies, contacts, particles and memory. The film carries on underneath.

## What the SDK offers, checked in the pinned checkout

PhysX SDK 5.13 (`physx/snippets`) and the two sibling projects in the same repository:

| Genre | SDK feature | GPU |
| --- | --- | --- |
| Rigid bodies | dynamics, continuous collision (CCD), triggers, SDF collision for concave meshes | CPU or CUDA |
| Joints and articulations | revolute, prismatic, gear, rack-and-pinion, mimic joints; reduced-coordinate articulations; fixed and spatial tendons | CPU or CUDA |
| Vehicles | vehicle SDK: four-wheel, tank, truck, custom tyres and suspension | CPU |
| Cloth | deformable surfaces | CUDA only |
| Soft bodies | deformable volumes, with attachments and skinning | CUDA only |
| Fluids | position-based fluids (PBF), multiple materials, isosurface extraction | CUDA only |
| Destruction | Blast (fracture and support graphs) | CPU |
| Smoke and fire | Flow (sparse-grid gas solver, combustion) | Vulkan compute |
| Scene queries | raycasts, sweeps, overlaps, BVH | CPU |

## The classic PhysX stack, box by box

The PhysX 2.x / APEX architecture (SDK, APEX, HAL, back-end solvers) maps onto today's SDK like this. Nothing in it is left out.

| Classic box | Today | In this plan |
| --- | --- | --- |
| SDK: Rigid Bodies | PhysX 5 rigid bodies | yes |
| SDK: Rag Dolls | reduced-coordinate articulations with joint drives | **added**: a ragdoll genre (see below) |
| SDK: Soft Bodies | deformable volumes | yes |
| SDK: Cloth | deformable surfaces | yes |
| SDK: Force Fields | removed after PhysX 2.8; rebuilt as per-body forces and Flow's velocity field | **added**: wind fields shared by cloth, smoke and fluids |
| SDK: Fluids | position-based fluids (PBF) | yes |
| APEX Particles | PBF particles; Flow for gas | yes |
| APEX Destruction | Blast, its successor | yes |
| APEX Clothing | deformable surfaces (NvCloth before that) | yes |
| APEX Fluids | PBF with isosurfaces | yes |
| APEX itself | discontinued; the last release (APEX 1.4) is open source inside NVIDIA's PhysX 3.4 repository | **optional**: an "archive" genre running APEX 1.4 itself, if it still builds (unverified) |
| HAL and back ends | x86 CPU and CUDA GPU remain. The PPU (Ageia's card), PowerPC and Cell solvers are gone with the hardware. | CPU and GPU readouts per genre |
| NaturalMotion | a separate company's product (euphoria), never part of PhysX, and not licensable here | not possible; the ragdoll genre uses PhysX articulations instead |

**Ragdolls and force fields in the story.** Ragdolls: the film has no human figures to give them, so they stay live only; mannequin figures tumble down the observatory stairs. Force fields: the terrace wind that moves the canopy ribbons and the pool, and the gusts that sway the tuned mass damper, all come from one wind field, so cloth, water, smoke and the pendulum answer the same gust.

## Each genre in the story

Every placement uses something already in the film.

| Genre | Where it plays | Film or live |
| --- | --- | --- |
| **Joints** | Taipei 101's **tuned mass damper**, which the film opens on (0:00): the real 660-tonne pendulum, hung on its cables and swayed by wind gusts, with the damper dashpots as joint drives. | Film, baked |
| **Articulations and tendons** | The **parcel winch** (2:47–2:55): the cable as a tendon-driven articulation, the case swinging and settling before the latch. | Film, baked |
| **Gear and mimic joints** | **Madame N's brass iris** (2:30–2:47): blades linked by gear and mimic joints, so the iris opens as a mechanism rather than a fade. The film's iris already has four shutter vanes, rails and knuckles (`optical_iris_vane` in `tools/gen_trilogy_meshes.py`, parts 220–223), so it needs pivots and joints, not new modelling. | Film, baked |
| **Cloth** | The **woven canopy ribbons** (2:10–2:30) moving in the terrace air. | Film, baked |
| **Fluids** | The **Neural Velvet reflecting pool** (1:40–2:10): disturbed by the film's own drifting petals (4 to 18 of them, by effects detail). The film bakes a ripple heightfield, not a mesh sequence. Live: particles, then an isosurface; rain on the terrace. | Both |
| **Soft bodies** | **Beneficial microbes** in the sanctuary ecology (2:55–3:15), squeezing between the planting. | Live first; film if it reads well |
| **Rigid bodies** | The Explore **physics marbles** move onto PhysX with CCD. Live: a heap of enamel tiles over the terrace, SDF collision against the orchid. | Live |
| **Vehicles** | Street **traffic** in Shenzhen and Taipei, under the eVTOL and the flock. | Live |
| **Smoke and fire** | Thermals rising under the **kettling kites** (built). Later: vapour from the **eVTOL's rotor downwash** and the **refuelling pair's** exhaust. Fire is not in yet: combustion is off. Baking smoke would cost megabytes a frame, so it stays live. | Live |
| **Destruction** | Live only: one of the **mirror stelae** shattering under a dropped marble. Kept out of the film unless you want it there, since the sanctuary is about balance. | Live |
| **Scene queries** | Never steer a bird (that would be PhysX flying it). Instead: an offline test that sweeps the authored bird routes and kettles against the city and fails on any hit, fixed by editing the route constants; and live collision for the bird camera and the Explore camera. | Offline test; live cameras |

The birds stay as they are: closed-form, ornithologically correct flight. PhysX supplies the air they ride (smoke) and keeps them out of buildings (queries); it never replaces their flight.

## Integration plan

Reviewed by Claude Fable 5.2 on 2026-11-21 against the code, with its main claims checked. File references come from that review.

### Shared foundation, built once

- **One runtime.** `include/boo/physx_suite.hpp` and `src/physics/physx_suite.cpp`, shaped like `FlowSmoke`, compiling to "unavailable" without `ENABLE_PHYSX`. It lives in the app layer beside today's marble solver (`stepMarbles` in `src/app/main.cpp`), because simulation is not a render pass. One foundation, physics object, CUDA context manager and CPU dispatcher for the process. Each genre gets its own scene, created when dialled in and released when dialled out, so only one genre holds GPU memory at a time.
- **One CUDA context.** The app already holds the GPU's CUDA context (`src/render/live_accelerators.cpp`). PhysX is handed it through `PxCudaContextManagerDesc::ctx`, a field that exists in 5.13, rather than creating a second.
- **Asynchronous start.** PhysX and Flow both start on a worker thread and show "starting" on the row, so dialling in never freezes the frame.
- **Stepping.** A fixed 1/120 s step with an accumulator, at most four steps a frame. Simulate this frame and read the results next frame, as Flow already does. Paused means no step, and a cut to another act resets the scene.
- **Dial and dashboard.** Everything today is wired for two states, off and smoke. The setting, the names, the diagnostics, the HUD constants and the stats type are widened together, and `FlowSmokeStats` gives way to a general `PhysxStats`: genre, state, backend, CPU step time, simulate-to-results time, render GPU time, four labelled counters and GPU memory. PhysX has no per-step GPU timer like Flow's, so the row says which time it shows.
- **Results to the renderer.**
  - **Rigid transforms**, used by most genres, go through a copy of the bird flock's fast path in `src/render/trilogy_renderer.cpp`, written straight into the frame's upload buffers. This needs a physics reserve in the instance capacity.
  - **Deforming meshes** (cloth, soft bodies, the fluid surface) need a dynamic vertex buffer and acceleration-structure refit, which the renderer lacks today. This is the largest piece of new renderer work in the suite.
  - **Particles and volumes** use a compute pass over the scene, as the smoke does.
  - **Readback first.** Data comes back through the CPU: about 0.6 MB a frame for 20,000 bodies and 1.6 MB for 100,000 particles. Sharing CUDA memory directly with Direct3D 12 comes later, only if a measurement calls for it.
- **Baked film physics.**
  - A headless `boo_physx_bake` tool runs the same scene code at the fixed step and writes `assets/physics/<shot>.bphys`, listed in the asset manifest. A track is rigid (position and quaternion), vertex (offsets from rest) or heightfield.
  - Playback is a pure function of film time, interpolating between keys, so seeking and motion vectors still work and the Store build needs no solver to play it.
  - The committed file is the truth: GPU solves differ between GPUs and drivers, so a re-bake is a content change.
  - Baked tracks respect Reduced Motion, which scales their amplitude about the rest pose.

### Corrections the review made

- **The film will change.** Every baked placement changes pixels in its shot, so the film-determinism baselines in `tests/integration` are regenerated once per placement. From here on, "bit-identical" means identical from run to run, not identical to today.
- **No collision world exists yet.** The marbles collide with hand-written floors and cylinders. Static collision must be cooked from the meshes offline and cached.
- **The Taipei 101 damper** is welded into the tower mesh (`tools/gen_trilogy_meshes.py`) and duplicated in the ray-marched and ray-traced scenes. All three copies must move together, which means splitting the mesh and regenerating it. Its real sway is a few centimetres, so it is more a tribute than a visible effect; if it is exaggerated, the user guide says so.
- **The parcel winch** today is one stretched line. A swinging case needs the line to rotate or to be drawn in segments, and the bake must end exactly on the dock pose the camera holds on.
- **The wind field** must come from the breeze the film already has, the sine breeze in `poseOrchid` and the canopy bob, or the baked ribbons and the orchids will answer different winds. (`planterGust` is the courier's wake on the planters, not wind.)
- **APEX 1.4** needs the old PhysX 3.4 tree and toolchain, and its GPU code will not run on an RTX 50-series card. If built at all, it is a separate CPU-only program.

### Each genre

| Genre | Mode | Data path | Row shows | Main risk |
| --- | --- | --- | --- | --- |
| Smoke (Flow) | Live | NanoVDB readback, ray march | sim ms, ray ms, voxels | start-up stall, memory |
| Rigid bodies, CCD, SDF | Live | marbles as today, heap through the fast path | bodies, awake, contact pairs, step ms, GPU MB | collision cooking |
| Joints: damper | Film | rigid track | sway, damping force | mesh split in three places |
| Articulations: winch | Film | rigid track | links, degrees of freedom, tendon length | landing on the dock pose |
| Gear and mimic: iris | Film | rigid track | joints, gear ratio, constraint error | vanes interpenetrating under Reduced Motion |
| Ragdolls | Live | rigid instances, about 15 links a figure | figures, links, contacts | mannequin mesh; stairs geometry unverified |
| Vehicles | Live | rigid instances | vehicles, wheels in contact, step ms | road collision, car meshes |
| Cloth: canopy | Film and live | vertex track | vertices, triangles, attachments, step ms | deforming-mesh path; about 14 MB baked |
| Soft bodies: microbes | Live | deforming mesh | tetrahedra, vertices, step ms | deforming-mesh path |
| Fluids: pool | Film and live | heightfield track; live particles | particles, iterations, step ms | surface rendering |
| Blast: stela | Live | one mesh per chunk | chunks, bonds broken, islands | separate library build; mesh count |
| Scene queries | Offline and cameras | nothing drawn | queries a frame, µs | none |
| Wind field | All | closed-form function | gust speed | matching the film's gust |
| APEX 1.4 archive | Separate program | its own | its own | old toolchain |

### Build order

1. **Foundation** (built 2026-11-21): the runtime, the shared CUDA context, asynchronous start for PhysX and for Flow, fixed-step timing, the genre table behind the dial, general stats. See "Step 1, as built" below.
2. **Rigid bodies and collision:** collision cooking, the marbles on PhysX (the analytic marble solver stays as the fallback for builds without PhysX), the fast instance path with a physics reserve wired through both the start-up allocation and the buffer growth, the tile heap. The **bird-route sweep test** comes here too: it needs only the cooked city and enforces the rule that PhysX never flies a bird.
3. **Wind field**, before anything is baked, so no track is baked twice.
4. **Baking:** the bake tool and rigid tracks, then the damper once its mesh is split, then the winch, then the iris.
5. **Deforming-mesh renderer path**, which has no PhysX dependency and can start beside steps 2–4: dynamic vertex buffers, recomputed normals, acceleration structures built to allow updates (refit for cloth and soft bodies, rebuild for the fluid surface, whose topology changes), and **per-vertex motion vectors**, without which DLSS, Ray Reconstruction and the denoiser smear deforming surfaces.
6. **Cloth, then soft bodies**, both after step 5. **Fluids:** live particles need only step 1; the pool's baked heightfield needs step 4.
7. **Optional, after the film-facing genres:** ragdolls, vehicles, Blast and APEX. Each needs art that does not exist yet (mannequin and stairs, cars and roads, a fractured stela) or an old toolchain, so none may hold up the rest.

Alongside these: the damper mesh split, fire in Flow, and the art for the optional genres.

### Second review (Antigravity CLI), 2026-11-21

A second read-only review checked the plan against the code and the SDK headers. Settled by it:
- **CCD** in a GPU scene: swept CCD runs on the CPU after the GPU solver; speculative CCD runs on the GPU (`PxSceneDesc.h`).
- **Wind on cloth:** deformable surfaces have no wind setting, so the wind field writes their velocities directly. Particle systems do have `setWind`.
- **GPU memory figures** exist in `PxSimulationStatistics` (`gpuMemHeap`, `gpuMemParticles`, `gpuMemDeformableSurfaces`, and others), so the row can report exact bytes.
- **Corrections taken into the plan above:** the iris vanes already exist; the wind field's real source; the drifting petals; the build order (route test and wind earlier, optional genres last); per-vertex motion vectors; the instance reserve through buffer growth; Store isolation.
- **Shared CUDA context:** PhysX needs the context current on the thread that creates it, and found the first attempt failing without that. Step 1 makes it current; CUDA contexts may be current on several threads at once, so this does not disturb the app's own CUDA work.

### Step 1, as built (2026-11-21)

- `include/boo/physx_suite.hpp`, `src/physics/physx_suite.cpp`: one PhysX 5.13 runtime, started on a worker thread, on the film's GPU matched by adapter identity, using that GPU's primary CUDA context. Verified on the RTX 5080 Laptop GPU: ready in about 0.3 s with the GPU solvers, and a falling-box check on GPU dynamics passes.
- `include/boo/physx_stepper.hpp`: fixed 1/120 s steps, at most four a frame, nothing owed while paused, cleared on a cut.
- `include/boo/physx_genres.hpp`: the one table of genres that settings, the dial, the row and diagnostics all read.
- `PhysxStats` replaces the smoke-only figures on the row and in diagnostics, which also report the runtime (`runtime.physx.runtime`).
- Flow now starts on a worker thread too; the row reads `STARTING` meanwhile.
- PhysX is built with the static C runtime to match this project. The runtime starts only when a genre that needs it is dialled in; none does until step 2.

### Progress, 2026-11-21

| Step | State |
| --- | --- |
| 1 Foundation | Built and tested (shared runtime, GPU solvers on the shared CUDA context, background start, fixed steps, genre table, general stats). |
| 2 Rigid bodies and collision | Built: rigid genre (3,000 GPU bodies on the real buildings, an accent with its own moment), city collision boxes from the renderer, bird-route sweep with a 5 m clearance (`--physx-route-test`; workout flock adjusted to pass; at the user's request the two Taipei film kites that came within 1.7 m were moved 1.5 m and 3.75 m across, `kTaipeiFlockNudge`, from the sweep's own smallest-move search, so all birds now pass in both cities). The Explore marbles roll on PhysX (`src/physics/physx_marbles.cpp`: CPU scene, continuous collision detection, the built-in solver's collision world as PhysX shapes, the countertop basin as a 4 mm height field from the shared surface in `include/boo/dawn_bathroom.hpp`); the runtime starts on the first spawn and takes the marbles over mid-roll about 0.2 s later, and the built-in solver stays as the fallback. |
| 3 Wind field | Built: `include/boo/wind_field.hpp`, the orchid breeze lifted out unchanged, driving the smoke, the fluid and the baked winch and damper. |
| 4 Baking | Built: `tools/physx_bake`, `.bphys` format and playback; parcel winch (distance joint, honest 4 cm swing), iris (gear joints, a mechanism keyed by the director's iris value), Taipei 101 damper (D6 pendulum with dashpot damping, split from the tower at load time so the committed mesh is untouched; Antigravity's recommendation, Codex unavailable on the account). All bakes repeat bit for bit. |
| 5 Deforming-mesh renderer path | Built (`src/render/trilogy_renderer.cpp`, `CanopyCloth`). The moving vertices are rewritten inside the scene's vertex buffer each frame (only the moving parts' ranges), so raster, ray queries and the path tracer read the same surface; normals and tangents are turned with the surface. Each moving part has a ring of eight bottom-level acceleration structures, refitted every frame from a rest-shape structure built at load (refit from rest, so quality never drifts; full rebuilds cost about 1 ms, refits nothing measurable), from compact inputs of their own (rebased indices, positions in an upload ring), so a frame still in flight keeps its own. Last frame's positions go to the motion-vector pass through a new root SRV (`t12`), so DLSS and Ray Reconstruction track the surface. NRD's per-draw motion for interactive denoising does not yet carry per-vertex motion. Topology-changing surfaces (the fluid surface) would need a rebuild path, not built. |
| 6 Cloth, soft bodies, fluids | Cloth built for the film: `boo_physx_bake cloth` runs PhysX's GPU deformable-surface solver on the four canopy lamellae (strips 12% wider than the gap between their tendons, pinned along them and at the feet, wind as pressure on each node's patch), recording a 21 x 7 grid per ribbon at 10 Hz over the two spans the ribbons are on screen (100–150 s and 275–338.5 s): `assets/physics/canopy_cloth.bphys`, 4.0 MB, grid tracks in `.bphys` version 2. Result: a steady sail-like billow about 10 cm deep (15 cm at most) with small trembles in the gusts; a smooth breeze cannot make fabric flap, since flutter needs the air itself simulated. GPU cloth does not repeat bit for bit (two bakes differed by up to 8 cm, and still by up to 5 cm with PhysX's enhanced determinism switched on), so the committed file is the truth. Soft bodies built live (`src/physics/physx_soft.cpp`, genre "soft"): 24 microbes on PhysX's GPU deformable-volume (FEM) solver, each its own cooked mesh (320 collision, 585 simulation tetrahedra; sharing one mesh's auxiliary data between bodies made them explode), Young's modulus 60 kPa, dropped in spaced rings onto the block, step about 4 ms; each drawn as the best-fit ellipsoid of its collision mesh (principal axes and extents, read back every frame) through the body instance path, which now takes a per-body stretch. Live cloth on the dial and the pool's baked ripples are not built. Live fluid particles built (8,000 GPU PBF particles). |
| 7 Ragdolls, vehicles, Blast, APEX | Not started. |

### Still open

- **Store.** Which live genres ship there is the owner's call. Baked film physics needs no solver.
- **GPU memory.** There is no budget yet for Flow and a GPU PhysX scene loaded together.
- **Package size.** Baked cloth and other vertex tracks add to the download.
- **HUD space.** The dashboard row has four numbers in the 512-byte constant buffer; each genre's own labels live in the shader. A genre that needs more must move the row's text out of the constant buffer.
- **Store isolation.** `ENABLE_PHYSX` is off by default and the Store package is built from `build/x64-release`, which never has it; the packaging script also requires NVIDIA signatures on NVIDIA runtime files, which self-built PhysX and Flow libraries lack. Keep it that way until the Store question is decided.
