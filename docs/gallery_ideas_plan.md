# Ideas from gpu-demo-gallery: plan

Source: [arkits/gpu-demo-gallery](https://github.com/arkits/gpu-demo-gallery), 44 GPU launch demos
(1997–2006) ported to Rust and wgpu. Read 2026-11-27. We take techniques and test habits only, never
their content or a look-alike of any of those demos.

Who does what: Codex and agy write and QA the code, one brief at a time. Claude checks the diffs and
test results and keeps packaging and the Store.

## Part A. Cross-GPU capture comparison (do first)

### Why

`scripts/capture_baseline.ps1` compares SHA-256 hashes, so it only works on one GPU and driver. The
Iris Xe "city missing" bug (every package from 1.0.0.0 to 1.1.12.0) could not be caught that way. The
gallery runs each frame on three backends and diffs pixels with a small tolerance
(`tools/compare_png.py`: max, mean, and the number of channel values more than 8 apart; it fails over
a mean limit). We want the same check across our GPUs.

### What to build

1. **`tools/compare_captures.py`**, Python standard library only (zlib and struct for PNG, plus BMP).
   - `compare_captures.py --ref DIR --run DIR [--max-mean M] [--max-over8 F] [--json OUT]`
   - Pairs files by relative path. For each pair: size check, then per-channel |a-b| ignoring alpha,
     reporting max, mean, the share of values over 8, and the share over 64 (big structural change).
   - Writes a heat-map PNG for each failed pair (|diff| × 4, clamped) next to the run.
    - Exits 1 when any pair goes over a limit or is missing. Volatile files (the five live-HUD captures
      in the baseline) are exempt only from pixel-difference limits; missing or invalid captures still fail.
   - Unit tests in `tests/` against tiny PNGs made in the test: identical, one pixel off, shifted,
     different size, and missing.
2. **`capture_baseline.ps1 -CompareTolerant`**: calls the tool. Hash compare stays as is for "nothing
   changed on this machine" refactors.
3. **Per-GPU references** under `diagnostics/golden/gpu/<adapter-slug>/` (local, gitignored), recorded
   with the existing `-Record` metadata (adapter, driver, source fingerprint).
4. **Cross-GPU mode**: compare the Iris Xe run and the 4060 run against the 5080 run at a loose limit,
   starting at mean 6 and over-64 share 2 %. Tune the limits from real runs and write them into the
   script. Exact limits matter less than catching an empty city, a black frame, or the test picture.
   Measured 2026-11-27: the RT ("preferred") and raster captures of the same moment on the 5080
   differ by mean 12.3 (over-64 share 1.4 %), so a GPU without RT must be compared with the 5080's
   `_raster` captures, never with `_preferred` ones. A swapped frame gave mean 55; 1.1.11 against
   1.1.12 on the 5080 gave mean at most 0.03 (all 102 stable files pass). The tool takes about
   3 s per 1920x1584 pair, 4 min 48 s for all 107 files.
   Add a sanity check that fails on any capture that is nearly one flat colour or matches the
   emergency test picture.
5. **Run on the HP Envy laptop** (RTX 4060 + Iris Xe, `ssh jeffr@MRKT.local`): quick set
   `--film-captures --film-capture-seconds 4,18,30,120,200,318 --no-activate`, on both adapters.
   GUI runs need the one-off interactive scheduled task, which is deleted along with the test folder
   afterwards.
6. **`run_smoke.ps1`**: an optional `-CompareRef DIR` switch that runs the tolerant compare after the
   capture suites. Off by default, so CI and the short smoke run stay as they are.

### Done when

- The tool's unit tests pass; the 5080 run against its own reference is clean.
- Iris Xe and 4060 runs against the 5080 reference pass with the tuned limits.
- Rebuilding 1.1.12.0's `m2_mesh*.cso` (the double-math shaders) makes the Iris Xe comparison fail.
  This proves the check catches the real bug. Revert afterwards.
- `docs/USER_GUIDE.md` or the README testing section gets three lines on how to run it.

### Briefs (one at a time)

- Codex, brief A1: items 1 and 2 with their tests. Files: `tools/compare_captures.py`,
  `scripts/capture_baseline.ps1`, a new unit test. No shader or engine changes.
- agy, brief A2: review A1's diff only (PNG decoding, the volatile list, exit codes). Narrow brief,
  since agy times out on wide ones.
- Claude: items 3 to 5 on the hardware (the runs, limit tuning, the regression proof).
- Codex, brief A3: item 6, plus the docs lines.

## Part B. Graphics ideas, mapped to our renderer

Checked against our shaders first. Things we already have and that are not repeated here: the
scattering sky table with Mie aureole and haze, cloud self-shadowing (`cloudKeyTransmission`),
anamorphic flare, ghosts, halation and grain, thin-film, RT refraction with dispersion, wrap and
back-light translucency, and NVIDIA Flow smoke.

Ranked by how much each one helps per unit of work.

### B1. Screen-space refraction for the raster path (from Box of Smoke, Crystal Ball)

`m2_mesh.hlsl` around line 3267: without `M2_RT`, glass such as aircraft canopies (material 18) draws
a flat dark interior, because "Raster has no refraction hit behind an opaque draw". Box of Smoke draws
the opaque scene to an image first, then glass front faces sample that image displaced by the
refracted normal, and Crystal Ball does it in passes too (back wall, contents, glass over the top).

- Copy scene colour and depth after the opaque pass, before transparent draws. Glass samples the copy
  at `uv + refractOffset(n, v, ior) * thickness`, rejecting samples whose depth is in front of the
  glass (fall back to the reflection).
- Only for non-RT GPUs, so Iris Xe, Arc and AMD iGPUs look closer to the RTX film. These are the
  machines Store reviewers are likely to use.
- Cost: one copy per frame; skip it when no glass is on screen.
- Check: Iris Xe captures of the canopy shots before and after, plus the Part A compare against the
  5080 RT frames. The gap should get smaller.

### B2. Before and after wipes in the Hardware Solos (from Car Paint, Natural Light)

Car Paint has chapters that show the same scene without and then with a normal map, and 8-bit vs
16-bit maps. Natural Light has a "HDR split" and an "exposure split". Our solos (RT, CUDA, Tensor,
NPU) state what runs, but a viewer can't see what it adds.

- A wipe line that moves across the frame: raster on one side and RT on the other, the same for DLSS
  off and on, and for neural shading off and on in the test rig only (no tensor cores in Store
  builds).
- Both sides render the same frame, with no fakes: it fits the honest-showcase rule. Each label names
  the real path.
- Cost: the RT solo already has both paths; this needs a split mask in the present shader and a cue
  key. Captions go in the language CSV, which Claude handles.
- Already there in part: the interactive captures include
  `interactive_bathroom_rtx_split_wipe_studio`, so an RTX split wipe exists in the interactive
  showcase. Start from that code and bring it into the film's solos, rather than writing a new one.

### B3. One-pixel lines at any resolution, with alpha to coverage (from Dawn, Grass)

Dawn draws 7,499 hairs as one-pixel lines with alpha to coverage, keeping their width at 1/768 of the
picture height. Grass lights blades as hair (normal = light direction minus its part along the blade)
and uses 20, 10 or 5 segments by distance.

- For the bird flock at distance, orchid stamens and cable or rigging lines: thin strands at a fixed
  share of the height, rather than in world units, so they don't shimmer or vanish at 1080p or grow
  thick at 4K.
- Hair-style lighting for thin strands, and segment level of detail by distance, to keep the
  200,000-bird budget (24 fps floor).
- Check: the bird shots at 1080p and 4K, flicker measured as the frame-to-frame difference of a
  steady shot.

### B4. An ink-outline look for one Midnight beat (from NPR Hatching)

NPR Hatching writes normals, depth and an object id into extra targets, then draws edges where any of
them jumps and thickens the lines in one last pass. Six hatching textures are weighted by N·L.

- A spy-noir graphic-novel moment: a few seconds in the Midnight edition, such as the dossier reveal,
  that blends the outline pass over the normal render.
- Only if it serves the story; one beat, not a mode.
- Cost: medium (two extra targets, one edge pass), so try it in the test rig first.

### B5. Two-tone flake paint for cars and the blimp (from Car Paint)

Car Paint's paint is a base coat that shifts colour with viewing angle, plus sparkle from a flake
normal map under a clear coat.

- Clear-coat lobe plus base-coat lobe, hue shifting with N·V, and flakes from a high-frequency normal
  noise at a fixed share of screen size.
- Only where cars or the blimp are on screen long enough to read it. Small cost.

### Not taken

- Sky and aerial perspective (Light Scattering, Grass): ours is already more complete.
- Precomputed radiance transfer (Subsurface Scattering): DXR covers it; heavy work for little gain.
- Idle autopilot and free cameras: Boo Sah Sah is a film with no free camera.
- A WebGPU teaser page: a good marketing idea but a separate project; it belongs to Demo-Verse.

### Order

The user asked for every sprint (2026-11-27), so the order is: A1, A3, then Part C, then B1 to B5.
Each one: a Codex brief, a read-only review by Sonnet (claude CLI), agy and Codex, fixes, then
Claude runs the captures and the Part A compare, and ships through the Store release policy.

## Part C. Intel UHD 620 (asked 2026-11-27)

The Store tester whose machine froze at launch (10.1.2.10) used a Dell Inspiron 5379, which has
Intel UHD 620 graphics (Gen9.5, 24 execution units, about a third of Iris Xe). The certification notes
call UHD 6xx "below the minimum". Getting it to play properly matters for certification.

Today integrated graphics start on the Safe preset (1280x720, 30 fps target, raster lighting), and
`QualityLadder::stepDown` (src/core/health.cpp) stops at Safe.

1. **A Lite tier below Safe.** Lower internal resolution (for example 960x540, upscaled by the
   existing present pass), effects detail low, the workout flock and PhysX genres off, cheaper
   clouds and sky marching, fewer light-shaft steps. The ladder steps down to it from Safe when
   measured frame times stay too long, and it is also the start preset for Intel Gen9 graphics
   (UHD 6xx, identified by PCI device id or by a short GPU-time probe at start-up).
2. **Feature check.** Every shader the raster and Lite paths load must need shader model 6.0 and no
   optional features. The `ShadersRunOnEveryGpu` unit test gets a list of those shaders and fails
   if any needs more. The RT shaders (ps_6_5) are loaded only when ray tracing is allowed.
3. **Memory.** Integrated GPUs share the PC's memory; keep the Lite tier's GPU memory inside about
   1.5 GB (the 5379 has 8 GB of RAM in all) and log what it uses.
4. **WARP as a stand-in.** `--warp-full-path` crashes with a heap fault (follow-up from 765da8a).
   Fix it: WARP has no optional features and no vendor driver, so the full film running on WARP at
   low resolution is a useful check that nothing needs more than a basic D3D12 GPU.
5. **How we test without a UHD 620.** On the laptop's Iris Xe: the Lite tier must hold at least 60
   fps at its resolution (UHD 620 at about a third would then give about 20 fps), and the tolerant
   compare must pass against the 5080's raster captures at a looser limit (Lite is softer). WARP:
   the full path renders the quick capture set without crashing. The certification notes are
   updated to say what was tested and how.

## Part D. Design round (2026-11-27)

The user asked for a creative, open-minded pass. agy, Sonnet and Codex each read the shaders, the
shot list and the Last Light spec and proposed ideas independently (reports kept in the session
scratchpad: design-agy.md, design-sonnet.md, design-codex.md). Where two or three of them reached the
same idea, it ranks higher.

| Order | Idea | Who proposed it | Where | Why |
|---:|---|---|---|---|
| 1 | Per-window skyline ignition | agy | 23-32 s | The Last Light spec promises it; today one global `night` value fades the whole city. Checked in the code. Every GPU. |
| 2 | A grade for each act (black point, split toning, highlight roll-off over film time, ASC CDL in `PSCompositeCanvas`) | Sonnet | All six minutes | Near zero cost, helps every frame on every GPU including UHD 620; lock it before tuning other looks. |
| 3 | The mirror witness: a real bird seen only in the stela's reflection | Sonnet (night heron), Codex (kite); agy's multi-bounce mirrors make it richer | 0:59.5 | Strongest new piece of the hidden bird story, on an honest RT path. Check the sightline first. |
| 4 | Multi-bounce Hall of Mirrors (bounded 3 bounces in `traceMirrorScene`) | agy | Act 2, RT solo | Raster cannot fake it; pairs with B2's wipe. RT only. |
| 5 | Focus as a handoff: authored rack focus with oval bokeh | Sonnet, Codex | 0:59.5, 2:47-2:59, 5:37 | Directs attention without captions. Mirror pixels need the reflected image's depth. |
| 6 | Birds at dawn beyond the bathroom window | Sonnet, Codex | 5:42-5:55 | Dusk gathers the birds, dawn disperses them; nearly free. Clear the end card. |
| 7 | Material details: handled latch, woven ribbon light, machined iris, enamel opalescence | Codex, agy | 2:10-2:59, enamel vaults | Cheap, close-up rewards; filter by pixel footprint so nothing sparkles. |
| 8 | Bathroom mirror condensation and dispersed caustics | agy | 5:35-6:00 | Bookend for the closing still; condensation runs on every GPU. |
| 9 | Gobo shafts through Madame N's iris, CUDA particles lighting the sanctuary | agy | Acts 4-5 | Big, but needs pass changes; keep the particle light modest (Sonnet). |

Dropped: the police searchlight on the egrets. Sonnet argued that it turns the hidden bird story
into a spotlit event, and egrets commuting at night is doubtful. B3 (alpha to coverage) is replaced
by analytic width expansion, because the renderer has no MSAA (agy). B4 (ink outlines) ranks lowest
for effort.

Order of work after D1: Part C (certification), D2 grade, B1, B2, D3 with D4, D5, D6, D7, D8, then
B5. Each gets the same brief, three-way review, and capture check.
