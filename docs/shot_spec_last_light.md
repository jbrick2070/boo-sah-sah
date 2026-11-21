# Shot specification: "Last Light" (film 0:00 to 0:45)

## Revision 2 (2026-11-21): 45 seconds, after reviews by Codex and Antigravity

The owner asked for the whole first 45 seconds and for the shot to be truly jaw-dropping. Codex (on the Mac) and Antigravity reviewed the 30 s draft below against the code; Claude checked their claims and judged. Where this section and the draft disagree, this section wins.

**Beats.** 0-7 s the inverted sky (tower and kites only in the pool's ray-traced reflection); 7-13 s the gust (the banner streams metres; its shadow crosses the pool); 13-23 s turn upwind and crane up, focus racked from the banner to the skyline, kettle and Taipei 101 together; 23-32 s last light to night (sun sets, `daylight` falls to 0, the city switches on window by window, directly and in the pool); 32-45 s descend to the orchid as it blooms (`revealOpen01` 28-38 s) and the enamel vaults rise (parts 210-215, 30.5-38 s), handing over through the existing 1.8 s blend in `evaluateShowCamera` (43.2-45.0 s) to the exact 45.0 s state.

**Decisions and corrections.**
- Replaces the whole `t < 45` branch of `evalFilmAt`; nothing after 45.0 s changes.
- Daylight starts at 0.70 (windows dark; `nightFactor = smoothstep(0.68, 0.08, daylight)`), holds to 16 s, falls to 0 by 31 s, so the lights-on is seen (Antigravity). Act 2's IBL (`sampleIblEnv`) blends toward the day slice with `daylight` in the opening.
- The pool (material 7) reflects through `traceSceneEx` with a roughness cone and ripple normals, not `traceMirrorScene` (both reviewers; checked at `m2_mesh.hlsl:2207`). In the opening, calm the ripples and skip the temporal jitter on material 7 so distant kites read.
- **Kettle: 32 birds**, not 96. Taiwan's 2025 synchronised survey counted 1,033 black kites nationwide, roosting mostly outside central Taipei (Codex, Forestry and Nature Conservation Agency). A modest kettle framed for scale is the honest choice.
- The film's kettle is its own fixed cast with its own flight frame, leaning along `kWindHeading`, independent of the Workout dial; the workout kettle is unchanged (Codex). Soaring wings stay still (`kettlePose` passes motion 0), which is correct for a thermal; the draft's "wings articulating" is withdrawn (Antigravity).
- No new texture slice for the silk: the banner is shaded by a part-keyed branch with a procedural weave and backlit transmission, avoiding +4.2 MB and the ten `min(..., 41u)` clamps (Antigravity).
- The blimp's crossing moves past 45 s, so the sky stays clean.
- Wormholes (offered by the owner): not in this shot. Its thesis is one wind moving cloth, birds and light; recursive portals would dilute it. They belong to the platform's world-to-world jump (`Ctrl+L`).
- Gate: the RTX 5080 Laptop at Showcase 1440p, DLSS on, Frame Generation off; check frame-time tails at the reflection, the gust and the hand-over.

---

*Draft 1 (30 s) follows, superseded where it disagrees with Revision 2.*

# Shot specification: "Last Light" (film 0:00 to 0:30), draft 1

Draft, 2026-11-21. A specification only: nothing here is built. Written against the code at 1.1.7.0 (commit `1ee2b16`). Every coordinate is in the film's world space as the code uses it (metres, Y up; +X is east and +Z is north, taken from the dawn sun comment in `assets/shaders/m2_mesh.hlsl` and the wind heading in `include/boo/wind_field.hpp`). Where a figure is an estimate or was not verified in code, it says so.

## 1. Concept

**Logline.** At last light the sky is first seen upside down in the sanctuary's reflecting pool: a gold tower and a slow wheel of black kites. A gust crosses the terrace, a six-metre silk banner lifts and streams, and the camera turns upwind to find the real birds circling beside Taipei 101 as the light goes and the city switches on.

**Emotional beat.** Stillness, then release, then night. It is the breath before the film's noir begins.

**Story and birds.** The film already opens on Taipei 101 with black kites nearby (`birdRoute(2, false)` is anchored at 12 s and its comment has them leaving "the observatory's high thermals" from 27 s). This shot makes that the subject instead of a background detail. The film then runs dusk, night, dawn: it ends on the dawn bathroom (5:42), so a dusk opening closes the day it starts. The first caption, at 4 to 14 s, already says "KEEP YOUR EYES UP, IRIS".

**Why dusk.** A kettle is thermal soaring and cannot happen at night, which is what the film's exterior is from 0:00 to 5:35 (`c.day` is 0 until 335.5 s). Black kites do gather and circle above their roost in the last light, on the heat the city has stored. That is the one time of day at which a kettle beside the tower is correct without moving the film's ending, so the shot takes it.

**Why a first-time viewer understands it.** One cause, three effects, in the order they are shown: the air moves, so the cloth moves, and the birds are riding the same air. Nothing needs a caption or a dashboard to read. The one wind is literal: the banner is baked against `windAt()` and the kettle is the existing closed-form `FlockFormation::Kettle`.

**Calling card (context, not scope).** A banner is a flag: in the linked-universes vision (`docs/community_platform_framework.md`) it is the obvious thing for each world to fly. Version 1 carries no emblem or text.

## 2. Placement

- **Film time 0.0 to 30.0 s**, in `ShowVisualMode::FullFilm360s` only. It replaces the camera move of the opening (shot-list rows "0:00-0:28 Taipei 101 observatory and Xinyi skyline" and the first two seconds of "0:28-0:45 Rooftop sanctuary"). It ends on exactly the pose the existing code holds at 30 s (eye (1.15, 5.55, 0.05), target (0, 5.45, 5.50), FOV 0.78), so everything from 30.0 s on is the film as it is: the enamel vaults rise at 30.5 s, the scale reveal is at 36 s.
- **Running time:** unchanged, 360.000 s. `assets/cues/film_360s.json` is unchanged (scene `taipei_intro`, samples 0 to 2,160,000, reveal at 1,728,000). No cue file changes at all.
- **Solo modes, M1 slice, M2 trilogy, finale loop:** unchanged. None of the new parts, the kettle, or the dusk light is active outside `FullFilm360s` with film time under 45 s. The four solo loops and the finale loop start at 45 s or later (`solo_loops` in the cue file).
- **What is lost:** the four-second close view of the tuned mass damper at 0:00. The damper still swings (its bake is untouched) and is still seen at 5:03 to 5:11.
- **What it reuses:** kit 2 (Taipei 101, the terrace, the pool), kit 3 (the orchid, still closed), the five authored kites, the kettle formation, the wind field, the deforming-mesh path and the `.bphys` grid-track format. No part of kit 4 is on screen before 30.5 s, so the terrace is a clean stage.
- **Rejected placement:** 4:35 to 4:55 ("Taipei terrace at night", a 20 s return orbit) is the easiest slot to replace, but it is night, where a kettle would be wrong. 5:01 to 5:31 (the crown orbit) at dawn was also considered; it collides with the finale's own light ramp (`isDawnFinale()`, `timeRamp(336, 346)`), which starts blue at 338.5 s.

## 3. Shot breakdown

World facts used (from `tools/gen_trilogy_meshes.py`, `src/render/trilogy_renderer.cpp`, `src/render/bird_flight.hpp`):

| Thing | Where |
| --- | --- |
| Terrace deck | x -7.8 to 7.8, z -2.4 to 8.8, top at y 2.63 |
| Reflecting pool (material 7) | x -5.5 to 5.9, z 2.45 to 8.25, surface y 2.665 |
| Orchid (kit 3) | (0, 5.85, 5.50), closed until 28 s |
| Taipei 101 (kit 2, scale 0.08) | axis (18.5, y, 60.0); observatory y 31.4 to 34.4; roof y 35.8; spire tip y 40.6 |
| Kettle, thermal 0, before 27 s | axis base (-2, 38.2, 34), leaning +0.18 m in x and +0.07 m in z per metre of height; with 96 birds the column is 20.2 m tall (y 38.2 to 58.4), circles of radius 7 to 18 m |
| Authored kites (lead bird) | (-15.0, 20.3, 19.0) at 0 s; (-8.9, 28.2, 39.2) at 9 s; (6.6, 26.1, 49.4) at 15 s; (11.7, 23.1, 48.5) at 18 s; (-4.5, 16.0, 21.5) at 30 s |
| Wind | blows toward (-0.62, 0, -0.78), from the north-east |
| New banner | top edge centre (-6.82, 10.30, 4.13); hangs to y 4.30; streams toward the south-west (section 5) |

Wind speed at the banner from `windAt()` (computed for this spec at (-6.8, 7.3, 4.1)): 1.52 m/s at 2 s, 0.57 at 7 s, **1.59 at 10 s**, 0.53 at 13 to 14 s, 1.48 at 19 s, **0.34 at 22 s**, 1.30 at 26 s, 0.47 at 30 s. The beats below are cut to this real gust curve; no special gust is authored for the shot.

Camera keyframes (film seconds; eye; target; vertical FOV in radians):

| Key | t | Eye | Target | FOV |
| --- | --- | --- | --- | --- |
| K0 | 0.0 | (2.90, 6.50, 1.50) | (1.40, 2.665, 4.85) | 0.95 |
| K1 | 5.0 | (2.60, 6.30, 1.90) | (1.20, 2.665, 4.60) | 0.95 |
| K2 | 7.5 | (0.40, 3.90, -1.50) | (-7.60, 7.90, 3.20) | 1.00 |
| K3 | 11.0 | (0.55, 3.95, -1.55) | (-7.60, 7.90, 3.20) | 1.00 |
| K4 | 17.0 | (-3.20, 3.30, -1.80) | (-0.38, 26.80, 30.44) | 1.02 |
| K5 | 21.0 | (-3.00, 3.35, -1.75) | (-0.20, 26.40, 30.40) | 1.02 |
| K6 | 30.0 | (1.15, 5.55, 0.05) | (0.00, 5.45, 5.50) | 0.78 |

Interpolation: within each interval use `smootherstep01` of the interval's fraction for eye and FOV. Interpolate the look **direction** (normalise target minus eye, spherical interpolation) and the look distance separately, then rebuild the target, as `frameAircraft` already does for the relay pan; a straight lerp between a target 4 m away and one 40 m away pans unevenly. No cuts inside the shot. The existing 1.8 s blend into 45 s is untouched. `revealOpen01` keeps its formula (`smootherstep01((t - 28) / 10)`). `shotIndex` stays 0. `focusWeight` stays 0 (scene autofocus). After 30 s, add the existing `irisShift * 0.20` to eye x, weighted in over 21 to 30 s so the handover at 30.0 is exact.

Second by second:

| Time | Camera | On screen | What moves and why |
| --- | --- | --- | --- |
| 0.0-5.0 | K0 to K1: above the pool, looking down about 50 degrees, a slow push | The pool as a mirror. In it: the upper third of Taipei 101 with its lit observatory (reflection lands at about x 4.0 to 4.3, z 7.0 to 8.1), the kettle turning (about x 2.4, z 4.4), the closed orchid standing in the water, and at the lower left the banner's reflection (about x -0.4 to -3.4, z 2.6 to 2.9; when it streams, part of it falls on the wet deck instead) | The gust that peaked at 2 s dies away: the banner's reflection sinks until the cloth hangs. The kites wheel. The banner's long shadow lies across the pool (sun 14 degrees up) |
| 5.0-7.5 | K1 to K2: boom down and back, tilt up off the water | The real banner comes into frame, hanging nearly still, backlit by the low sun just outside frame left; city and ridges behind | Lull (0.57 m/s at 7 s). Camera settles before the gust so the motion that follows is the cloth's alone |
| 7.5-11.0 | K2 to K3: locked off, 16 cm of drift | The banner fills the middle of the frame. View direction is 82 degrees off the wind, so it streams straight across the screen, right to left | **The PhysX moment.** Wind rises from 0.57 to 1.59 m/s in three seconds. The banner swings from about 30 degrees off the vertical to about 68, its tip travelling roughly 3 to 4 m, with waves running down its length and the silk glowing where the sun comes through. Caption 01 is up (4 to 14 s) |
| 11.0-17.0 | K3 to K4: pan right 65 degrees and tilt up 13, moving 3.6 m west along the south edge (average 11 degrees a second) | Turning upwind. The banner slides to the left edge; the tower enters right; the kettle rises into the top of the frame | The camera goes where the wind comes from. Second lull at 13 to 14 s, so the banner sags as it leaves centre |
| 17.0-21.0 | K4 to K5: hold, slight drift | The whole world in one frame: banner top streaming at the far left (about 36 degrees left of centre), the kettle above centre (elevation 44 to 57 degrees, about 50 degrees wide), Taipei 101 to the right (14 degrees right, top at 30 degrees). By the route table the five authored kites cross in front of the observatory between 15 and 19 s (bearing 11 to 17 degrees, elevation 20 to 24) | Gust peak at 19 s. Sun is on the horizon; the last gold leaves the tower. Windows come on (`nightFactor` in the shader rises as `daylight` falls) |
| 21.0-30.0 | K5 to K6: tilt down 37 degrees and drift to the terrace pose | Kettle and tower leave the top of frame; the dark terrace and the closed orchid arrive | Deepest lull at 22 s (0.34 m/s): the banner falls slack as the light goes. From 25 s the film's own night key fades in. At 28 s the orchid begins to open; at 30.0 s the old film resumes |

**Reduced Motion** (`motionScale` below 1): eye, direction and FOV are each `lerp(reduced, full, motionScale)`, where the reduced camera sits at K4 from 0 to 22 s and eases to K6 by 30 s. The banner plays its calm track (section 5) and the kettle keeps flying, since `workoutFlockFrame` already scales its time by the motion setting.

Not yet checked by capture, flagged here rather than assumed: the blimp crosses at 13 s (`kSkyBlimp`, at about (26, 32, 104) then), which puts it low beside the tower in the K4 frame; and the airliner at y 41. Both are existing film traffic. See open question 9.

## 4. Technologies

Two technologies make the shot; the third is its subject.

### 4.1 Ray tracing, path tracing and DLSS Ray Reconstruction: the sky in the water

- **What the viewer sees.** The first five seconds are almost entirely a reflection: tower, birds, flower and silk, upside down in black water, each bird's wings articulating. Then the low sun throws the banner's moving shadow across the pool and the deck, and lights the silk from behind.
- **Why it is not subtle.** The reflection is the image, not a detail in it: about 60% of the frame is pool in K0 to K1. With ray tracing off the pool cannot show the kettle at all (it is far off screen, 35 to 50 m up), so the RT A/B split shows birds on one side and empty water on the other.
- **How it works today.** Mirror reflections trace to 180 m against instance mask 1 (`traceMirrorScene` in `m2_mesh.hlsl`); the kettle is 36 to 62 m from the pool. Flock birds are in the top-level structure every frame (`flockRays` in `trilogy_renderer.cpp`). To verify in step one of the build: that the kite materials carry mask 1 (opaque) so they appear in mask-1 reflection and shadow rays.
- **Dashboard.** The existing rows, unchanged: `PATH TRACING` (level), `DLSS UPSCALING`, `RAY RECONSTRUCTION`, `FRAME GENERATION`, each `LIVE`; the hardware table's RT row `LIVE`. No new label.

### 4.2 PhysX cloth, baked: the banner

- **What the viewer sees.** A 1 m by 6 m silk banner that hangs, then streams. Tip travel of metres, not centimetres: this is the correction to the canopy's 10 cm billow.
- **Why it is unmistakable yet an accent.** It owns three and a half seconds (7.5 to 11.0) with a still camera and nothing else moving in frame. Before and after, it is a reflection, an edge element, or out of frame. It is one object on one corner of the terrace.
- **Why the motion is large and honest.** The existing canopy is 0.30 kg/m2 fabric pinned on all four sides, so it can only belly. This is light silk (0.045 kg/m2, about 10 momme) pinned along one edge. Its weight is about 0.44 Pa; the wind's pressure at 1.59 m/s with the bake's own drag term is about 1.8 Pa. Balancing the two on a hanging sheet gives roughly 68 degrees off the vertical in the gusts and 13 to 30 in the lulls (hand estimate; the bake decides). No wind is exaggerated: the wind field is used as it is.
- **What it is not.** The bake's wind is a pressure on each node along the cloth normal. It does not simulate the air, so it produces swing, belly and travelling waves but not true flag flutter (`docs/physx_suite_plan.md` already says so). The user guide must describe it that way.
- **Real technique.** PhysX 5.13 GPU deformable surface, simulated offline by `boo_physx_bake`, played back from `assets/physics/wind_banner.bphys` through the deforming-mesh path, so raster, ray queries and the path tracer see the same cloth and DLSS gets per-vertex motion. Store builds need no PhysX runtime.
- **Dashboard.** The `PHYSX` row currently shows `N/A` in Store builds even while baked physics is on screen. New state: when the dial is Off (or the build has no SDK) and a baked track is moving visible geometry, the row reads `PHYSX  BAKED  CLOTH  287 NODES  30 HZ  3.4 M` (nodes, key rate, widest offset this frame in metres). This also covers the canopy (`canopy.lastOffset` already exists for it). It never reads `LIVE` for a bake.

### 4.3 The flock: 96 ray-traced kites in a kettle

- **What the viewer sees.** Ninety-six black kites circling up one thermal, all turning the same way, wings held out, gliding out at the top and rejoining at the base, exactly as the Workout dial's kettle flies (`bird_detail::kettlePose`), with the film's five authored kites crossing below them. At 36 to 50 m a 1.66 m wingspan is roughly 50 to 65 pixels at 1440p, so each bird reads as a bird.
- **Why 96, not 4,000.** The film's own birds must be correct, and Taiwan's black kite population is in the hundreds (figure from memory; verify before the user guide states it). A pre-roost gathering of about a hundred is a real sight; a kettle of thousands over Taipei is not. The 200,000-bird dial stays what it is, a labelled workout.
- **Dashboard.** `FLOCK  KETTLE  MILVUS MIGRANS` as today, with `FILM` in place of the click hint while the film's kettle is flying. The Workout row still shows only the birds the dial released (0 at level 0). Dialling the workout up during the shot grows this same kettle.

## 5. Assets and bakes

### 5.1 Meshes (built at load, like the parcel hardware; no Blender regeneration, no change to `assets/meshes`)

New in `include/boo/city_modules.hpp` and `src/render/city_modules.cpp`:

```cpp
inline constexpr uint32_t kBannerMast = 190, kBannerCloth = 191;   // 190..199 are unused today
Bmesh buildWindBanner(uint32_t part);                              // world coordinates, kit 2
```

- **`kBannerMast` (190):** a bronze mast, radius 0.06 m, from (-6.60, 2.63, 4.40) to (-6.60, 10.45, 4.40); a 0.35 m bracket from its top toward the wind's heading; a yard 1.04 m long, radius 0.02 m, centred on (-6.82, 10.30, 4.13) along (0.78, 0, -0.62) (square to the wind); a base collar. Material 38 (anodized dark bronze), collar material 8 (brass). About 600 triangles. The mast stands 0.95 m inside the west coping and 1.1 m west of the pool, clear of the planter (x -6.8 to -4.4, z 6.55 to 8.05), of every kit 4 foot (nearest: the stela at (-4.42, 5.03)) and of the courier's circuit (x 4 to 17).
- **`kBannerCloth` (191):** a two-sided sheet, 81 by 13 vertices a side with a closed rim, 0.6 mm apart, laid out on the rest shape below; texture coordinates are u along the length (0 at the yard) and v across, which is how the renderer maps vertices to the bake grid (the canopy's method). About 3,900 triangles. Material 42 (new).
- **Rest shape**, shared by the bake and the renderer in a new `include/boo/wind_banner.hpp`: `bannerRestPoint(u, v) = (-6.82, 10.30, 4.13) + (v - 0.5) * 1.0 * (0.78, 0, -0.62) + u * 6.0 * (0, -1, 0)`, u and v in 0..1. The header also holds the grid sizes, track names, key rates and the film span, as `canopy_cloth.hpp` does.

### 5.2 Material and textures

- **Material 42, "vermilion habotai silk"**: `tools/gen_pbr_textures.py` `NUM_MATS` 42 to 43; one more slice in each of `pbr_albedo_alpha.btex`, `pbr_normal.btex`, `pbr_ormt.btex` (about 1.4 MB each, so about +4.2 MB). Albedo vermilion (linear about (0.62, 0.07, 0.035)) with a fine plain weave; roughness 0.45; `MAT_TILE_METERS[42] = 0.30`. The renderer's fallback arrays go from 42 to 43 layers (`buildFallbackAlbedoArray(42u)` and its two siblings).
- **Shading** (new branch in `m2_mesh.hlsl`): a thin diffuse sheet with transmission. Front: ordinary diffuse and a soft sheen. Back: `sunRadiance * albedo * 0.55 * saturate(dot(-n, L))`, shadowed by one shadow ray. Ray instance mask 1 (opaque), so it casts a shadow and appears in the pool. Material 21 was not reused: its shader branch samples the Tensor SIREN field and it is masked as glass.
- Midnight edition: no change to the banner in version 1.

### 5.3 The bake

`boo_physx_bake banner` (new mode in `tools/physx_bake/physx_bake.cpp`, test-rig builds, needs CUDA) writes `assets/physics/wind_banner.bphys`: version 2, no rigid tracks, two grid tracks.

| Setting | Value |
| --- | --- |
| Sheet | 1.0 m by 6.0 m; simulated grid 81 along by 13 across (1,053 nodes, 1,920 triangles); stored grid every other node, 41 by 7 = 287 nodes |
| Pinned | the 13 nodes of the top row, to the yard |
| Fabric | areal density 0.045 kg/m2; `createDeformableSurfaceMaterial(2.0e7, 0.3, 0.4, 0.002, 2.0e-5)`; bending damping 0.02; linear damping 0.02 (starting values, taken from the canopy's and lightened; tune only within the limits below) |
| Solver | GPU dynamics, TGS, 24 position iterations, step 1/240 s for this bake (the tool's 1/120 s is likely too coarse for fabric this light; verify), self-collision on |
| Colliders | static capsules for the mast and the yard; a static plane at y 2.63 (the cloth should never reach it) |
| Gravity and wind | (0, -9.81, 0); wind exactly as the canopy bake applies it: `windAt(node, t)` relative to the node's velocity, as pressure `0.5 * 1.2 * 1.2 * patch * across * |across|` along the node normal, with `patch = 6.0 / 1053` m2 |
| Warm-up | 12 s before the first key |
| Track `banner_wind` | first key 0.0 s, last 45.5 s, 30 keys a second: 1,366 keys, 287 nodes, int16 offsets at 1/2000 m (range 16.4 m): about 2.35 MB |
| Track `banner_calm` | the same with the wind speed multiplied by 0.45, 15 keys a second: 683 keys, about 1.18 MB. Played under Reduced Motion |
| File | about 3.5 MB |

Reduced Motion plays the calm track rather than scaling offsets, because scaling the offsets of a sheet that has swung through 68 degrees would shrink the cloth by up to 16%. The rule: `cityFlightMotion(f) >= 0.75` plays `banner_wind`, anything lower plays `banner_calm`.

Acceptance of the committed file (checked by a unit test, section 9): the bottom-centre node moves at least 3.0 m between 7.0 and 10.5 s; it rises above y 7.5 at some time in 9.5 to 11.5 s; no pinned node is offset by more than 1 mm; no stored node comes within 0.05 m of the mast or below y 2.9; no stored grid edge is stretched or shortened by more than 5%. If the swing falls short, the allowed changes are the areal density (down to 0.030) and the length (up to 7 m). The wind is shared and is not to be changed for this shot. GPU cloth does not repeat bit for bit: the committed file is the truth and a re-bake is a content change.

Package size: about +7.7 MB in all.

### 5.4 Strings

No caption changes: captions 01 and 12 (the Family and Midnight openings) stay. One new key in all 25 files of `assets/strings`: `hud.state_baked`, text `BAKED`, note "Short status badge: this motion was simulated offline with NVIDIA PhysX and is being played back", `max` 12, `maxWide` 5. To check during the build: the Workout, Flock and PhysX rows currently draw their words as ASCII glyphs in `m1_taipei_slice.hlsl` and not from the label atlas; the badge should go through the atlas like `hud.state_live`, and the atlas has to have a free cell (`UiTextAtlasLayout`, 18 rows by 4 columns).

## 6. Code changes

| File | Change |
| --- | --- |
| `include/boo/wind_banner.hpp` (new) | Rest shape, grid sizes (`kBannerSimU = 81`, `kBannerSimV = 13`, `kBannerGridU = 41`, `kBannerGridV = 7`), track names, key rates, span 0 to 45.5 s, `bannerTrackFor(motion)` |
| `include/boo/last_light.hpp` (new) | `kLastLightEnd = 30.0`; `lastLightDay(t) = 0.36 * (1 - smootherstep((t - 10) / 16))`; `filmKettleBirds(mode, t, preset)`: 0 unless `FullFilm360s` and t < 45, else 48 on Safe and 96 otherwise, with the last second (44 to 45 s) easing the count to 0. Header-only so tests can call them |
| `src/render/m1_scene.cpp` | In `evalFilmAt`, when `t < 30.0`, the keyframed camera of section 3 replaces the body of the `t < 45.0` branch; from 30.0 the existing code runs unchanged. Add `FilmCameraInsert::LastLight` to the enum in `include/boo/m1_scene.hpp` and set it (nothing reads `insert` today; it is there for the HUD and tests) |
| `include/boo/city_modules.hpp`, `src/render/city_modules.cpp` | `kBannerMast`, `kBannerCloth`, `buildWindBanner` |
| `src/render/trilogy_renderer.cpp` | (a) append the two banner meshes beside the parcel hardware (`appendKit(buildWindBanner(part))`), kit 2. (b) `isPartActiveInShow`: parts 190 and 191 are active only in `FullFilm360s` with time < 45; return false for them first, before the solo branches. (c) `transform`: for these parts x = y = z = 0 and scale 1 (they are authored in world space), the same pattern as `kParcelDock`. (d) Deforming path: turn `CanopyCloth` into a small descriptor-driven `DeformingSurface` (ribbons, grid size, part predicate, track lookup, `Refit` or `Rebuild`) with two instances, canopy and banner. The banner **rebuilds** its bottom-level structure each frame (`PREFER_FAST_BUILD`) because a refit from the rest shape degrades when the surface has moved by metres; the canopy keeps its refit. Normals for the banner come straight from the deformed grid (plus or minus by face), not from `rotateBetween`, which gives up near a half turn. (e) The motion pass's previous-position view (`t12`) carries one vertex range; assert that at most one deforming surface is active in a frame (banner: under 45 s; canopy: 100 to 150 s and 275 to 338.5 s; never together). (f) Flock: after `flockPlan.birds` is set, if `filmKettleBirds(...) > 0` force `formation = Kettle`, `species = BlackKite`, and `birds = max(film, workout)`; with the workout at 0 pass `flockSize = float(birds)` and a fully grown arrival. (g) `c.day = lastLightDay(t)` for `FullFilm360s` and t < 30 |
| `assets/shaders/m2_mesh.hlsl` | `bool duskOpening() { return (showFlags & 65u) == 65u && act == 2u && time < 30.0 && !sunOverridden(); }` (bit 64 is set only by the full film, so the solo loops are excluded). In `sceneSunDir()`: before 25 s, `(-0.946 * cos(e), sin(e), -0.326 * cos(e))` with elevation `e = lerp(14 deg, -4 deg, smootherstep((time - 2) / 26))` (a west-south-west winter sunset); from 25 s the film's night key direction. In `sceneSunRadiance()`: before 25 s, `float3(4.35, 3.12, 1.98) * smoothstep(-0.04, 0.10, dir.y) * (1 - smoothstep(22, 25, time))` (the low-sun colour the Explore sun already uses); from 25 s the night key times `smoothstep(25, 30, time)`. The direction changes while the light is at zero, so shadows never jump. Plus the material 42 branch |
| `tools/physx_bake/physx_bake.cpp` | `bakeBanner()`, the `banner` argument, default output path |
| `tools/gen_pbr_textures.py`, `tools/gen_trilogy_meshes.py` | Material 42 slice; `MAT_TILE_METERS[42]` |
| `src/physics/physx_route_test.cpp` | A third sweep: the film kettle, every bird, counts 48 and 96, film time 0 to 45 s every 0.1 s, reported as "film kettle" |
| `src/render/m1_taipei_slice.cpp`, `assets/shaders/m1_taipei_slice.hlsl`, `include/boo/physx_suite.hpp` | `PhysxStats.state` gains 4 = baked (the HUD packs `min(state, 3) << 4`; widen it), and the row text of section 4.2. The renderer reports which baked surface is live and its node count, key rate and widest offset |
| `assets/strings/*.json` (25 files) | `hud.state_baked` |
| `assets/manifest.json` | New and changed assets, via `tools/refresh_asset_hashes.py` |
| `docs/film_shot_list.md`, `docs/USER_GUIDE.md`, `docs/physx_suite_plan.md`, `CHANGELOG.md` | The new opening; "the film's own birds" now include a kettle of 96 in the first 45 s; the banner in the list of baked film physics, described honestly (swing and waves, not flutter) |

**Determinism and seeking.** Every new quantity is a pure function of film time and settings: the camera (closed form), the light (`time` in the shader, `lastLightDay` in C++), the kettle (`workoutBirdPose` with a fixed count; no governor, no arrival front), the banner (`BakedGridTrack::sample`, Catmull-Rom between keys). Nothing integrates from frame to frame. A seek sets `dlssReset`, which already makes the deforming path send current positions as previous ones, so there is no motion-vector streak. The frame-rate floor never touches the film's kettle: the governor owns only `f.workout.birds`.

## 7. Audio

- **Placeholder: `assets/audio/01_neon_orchid.flac`, its first 30 seconds.** No work is needed for this: the cue files do not reference audio files (`audio_section` is a label), the bundled album starts on track 1 at film start and plays through (228 s, then track 2), and the engine keeps songs continuous across seeks. Nothing new is generated and no cue timing is added.
- Consequence to accept for now: the music is not cut to the picture, so the gust at 7.5 to 10 s lands wherever the track happens to be.
- **Brief for a composer, later.** 30 seconds, to picture, leading into the existing opening. 0 to 5 s: near silence, one held tone and room air (the water). 5 to 7.5 s: a single rising figure as the camera lifts. 7.5 to 10.5 s: the gust, a swell rather than a hit, peaking at 10.0 s, no percussion. 11 to 17 s: the turn upwind, a slow widening chord. 17 to 21 s: the widest harmony of the cue, the tower and the birds. 21 to 30 s: thin out to one low note under the first city sounds, landing on the downbeat of the current track's entry at 30.0 s. Flash-safe: no accent faster than 2 Hz driving the picture (`evaluateM1MusicalEnvelope` stays as it is).

## 8. Performance budget

Target: the shot holds the preset's frame rate on an RTX 5080 Laptop GPU with no Frame Generation, and costs no more than **2.0 ms of GPU time over the present opening** at Showcase (2560x1440, DLSS Quality, path tracing as the preset sets it). Estimates, to be replaced by timestamps (`docs/quality_timings.json` has the method):

| Item | Budget | Basis |
| --- | --- | --- |
| Banner pose (CPU) | 0.05 ms | 2,106 vertices; the canopy poses about as many |
| Banner acceleration-structure rebuild | 0.20 ms | The plan measured about 1 ms for rebuilding all four lamellae; this is one smaller surface |
| Banner silk shading | 0.25 ms | One extra shadow ray on at most a fifth of the pixels |
| Kettle, 96 birds (480 instances) | 0.05 ms | The workout measured about 105,000 birds at 28 fps, so well under a microsecond a bird |
| Pool reflection filling the frame, 0 to 5 s | 1.0 to 1.5 ms | Existing code, but far more mirror pixels than the old opening had. The largest and least certain item |

Presets:

| Preset | Behaviour |
| --- | --- |
| Safe (720p, 30 fps, no ray tracing) | Kettle of 48; three authored kites as today; banner plays the same bake. The pool is the raster substitute and cannot show the kettle, so the first five seconds are weaker; the RT row reads `SUB` as it does everywhere else. Nothing is faked to cover it |
| Balanced (1080p) | Full shot, kettle 96 |
| Showcase (1440p) | Full shot, kettle 96. The reference for the budget |
| 4K Experimental | Full shot. If it misses the frame rate, the existing health ladder steps the preset down (`src/core/health.cpp`); no shot-specific scaling |

Weaker GPUs: nothing in the shot scales with the machine, by design (determinism). The only load that does is the Workout dial, which the user controls.

## 9. Verification

**Captures** (native 1440p, Family and Midnight, RT and raster), added to `tests/integration/film_capture_gate.hpp`: 2 s (reflection), 6 s (banner hanging, in reflection and entering), 9 s and 10.5 s (the gust), 14 s (the pan), 18 s (the wide), 22 s (slack banner, lights on), 26 s, 29 s. One 10-second video from 5 s (`--hero-video-capture --capture-start-seconds 5`). The existing captures at 18 s and the validation frame at 15 s change and are regenerated; 36 s and 45 s should be compared before and after, since the mast may appear in a reflection.

**Unit tests** (`tests/unit`):
- Camera: `evaluateShowCamera` at 29.999 s and 30.0 s agree to 1 mm and 0.001 rad, and at 30.0 s equals the pre-change pose; continuous at every key; identical on repeated calls; at `motionScale` 0 it is the reduced camera.
- Light: `lastLightDay` is 0.36 at 0 s, 0 from 26 s, monotonic.
- Kettle: `filmKettleBirds` per mode and preset; 0 in every solo mode and in the trilogy.
- Bake file: loads; two grid tracks of 287 nodes; covers 0 to 45 s; all the acceptance checks of section 5.3.
- Mesh: `buildWindBanner(kBannerCloth)` texture coordinates cover the 41 by 7 grid, and its vertices match `bannerRestPoint` to 1 mm.

**Route test:** `--physx-route-test` must report no hit for "film kettle" at the 5 m clearance (`kBirdBuildingClearance`), nor for the authored birds as before. If it reports one, the fix is flight data (the thermal's base or lift), never the flight.

**Integration:** two runs of the full film give identical canonical frames at 9 s and 18 s; a seek from 200 s to 9.0 s gives the same frame as playing to it; the film stress gate passes; a Store-configuration build (`build/x64-release`, no `ENABLE_PHYSX`) plays the shot with the PhysX row reading `BAKED`.

**Done means:**
1. A person who has never seen the film can say, after one viewing of the first 30 seconds with the dashboard hidden, that the wind moved the flag and the birds were circling in it.
2. The banner's tip visibly travels more than 3 m in the gust, in the film, not only in the file.
3. The kettle and the tower are recognisable in the pool before the camera lifts.
4. The film from 30.0 s is pixel-identical to today's apart from the regenerated baselines named above.
5. All tests above pass, on the Store configuration as well.
6. Frame time over the shot is within the 2.0 ms budget at Showcase on the RTX 5080 Laptop GPU, measured.

## 10. Risks and open questions

1. **This replaces the film's opening.** It is the strongest slot for a calling card and the only one where a kettle is correct, but the opening is yours. The damper close-up at 0:00 goes. Approve, or name another slot (and accept the night problem there).
2. **Dusk light does not exist yet.** Taipei's exterior has never been rendered at `daylight` between 0 and 0.84 outside the finale's own branch. The values in section 6 are a first grade; expect to tune them against captures. Time is compressed (a sunset in 25 seconds), as the film's dawn already is.
3. **The kettle leans the wrong way.** `kettlePosition` leans the column toward +x and +z (east-north-east) while `kWindHeading` blows toward the south-west, so the banner will stream one way and the column lean the other. In a shot whose point is one wind, this should be fixed (lean along `kWindHeading`), which moves every kettle bird, so the route test and `cityFlockLift` must be re-measured. Your call; without it the shot still works but is wrong to a careful eye.
4. **Kites at night elsewhere.** The existing insert at 3:52 shows kites flying at night, and the film's kettle here keeps flying, off camera, until 45 s. Out of scope, noted because correctness is a rule.
5. **The count of 96** rests on my recollection of Taiwan's black kite numbers. Please confirm or give the figure you want.
6. **The banner exists only in the first 45 seconds.** That matches how kit 4 parts come and go by act, but it works against "one world". Keeping it for every terrace scene needs about 28 MB of bake at 30 keys a second (or a lower rate), and changes every terrace baseline. Version 1 or later?
7. **The cloth may not swing as estimated.** The 68 degrees is a hand calculation. Fabric this light may need the smaller step and more iterations to stay stable, and the pressure-only wind gives no real flutter. The acceptance numbers in section 5.3 are the gate; if the solver cannot meet them honestly, the fallback is a heavier, shorter banner with a smaller swing, not a faked one.
8. **Caption 01 says "XINYI RAIN ON GLASS"** over a clear dusk with soaring birds. The pool's rain rings are subtle and stay. Keep the line (it reads as rain just past), or rewrite captions 01 and 12 in 25 languages?
9. **The blimp** passes low beside the tower at about 13 s and may clutter the wide frame. Keep, or delay its crossing?
10. **Music is unsynchronised** (section 7). For a standalone clip you may prefer one of the other three tracks via `--audio-file`; I cannot judge them by ear.
11. **A new material slot** costs about 4.2 MB. The cheaper alternative is a part-keyed tint on an existing slice, which would look less like silk.
12. **The film kettle's exit at 44 to 45 s** shrinks the birds away over one second. The film camera cannot see it (the kettle is above frame from 31 s), but Explore and the bird camera can. An authored glide down to roost would be better and is not specified here.
13. **Reflection of the kettle depends on the kite materials' ray mask** and on mirror shading of distant thin wings at reflected distance; if they shimmer, Ray Reconstruction on or off at 2 s is the first thing to compare.
14. **HUD space.** The PhysX row has four numbers in a 512-byte constant buffer and its words are shader glyphs; the `BAKED` state has to fit there, and a localized badge needs a free atlas cell.
