# City graphics upgrade

Phase 0 records what commit `5ff4665` renders. The graphics target is a Taipei skyline with recognizable landmark spacing, a readable aircraft against the Shenzhen skyline, and street-level detail that holds up as the camera moves. Keep the seven-scene, 360-second film, its camera and soundtrack, both editions, Explore, optional accelerators and clean Spout output as acceptance references.

The instancing pass has matched city frames, draw counts, ray-tracing and raster measurements for the two procedural districts. [Phase 1 measurements](baselines/city-phase1/README.md)

## Which shots to compare

Always identify both cue and timestamp. The same sample in two cue sheets can show a different place. The film's seven scene boundaries are 0, 45, 100, 150, 195, 245, 275 and 360 seconds. Its three authored reveals occur at 36, 175 and 342 seconds. [Film cues](../assets/cues/film_360s.json) · [Trilogy cues](../assets/cues/m2_trilogy_180s.json) · [Camera paths](../src/render/m1_scene.cpp) · [Active geometry and transforms](../src/render/trilogy_renderer.cpp)

| Cue and interval | Shot and what must survive |
| --- | --- |
| Film, 0–45 s | Inspect the Taipei 101 observatory and Xinyi on the opening descent. At 8 and 16 s, judge skyline spacing, silhouettes, streets and distance haze. The scale reveal starts during the move onto the terrace. |
| Trilogy, 0–60 s | Shanghai appears in this presentation. At 15 s the eye is inside the rail truss, at roughly `(-0.52, 0.84, -30.79)`. At 45 s it is beyond the north portal, at roughly `(-1.48, 2.38, 12.11)`. Preserve the open river view beyond the portal at Z = 6 and the track corridor between the side frames at X = ±4.55. |
| Trilogy, 60–120 s and film, 245–275 s | Shenzhen cruise and approach use different camera functions in the two cues. Trilogy 75 and 108 s cover cruise and hover. Film 258 s covers the Signal Hijack tracking shot. Protect the swept volume of the six rotors, the hero pad around `(0, 3.64, -2)`, and the aircraft silhouette against the city. |
| Trilogy, 120–180 s | At 168 s the camera frames the Glass Orchid close up. At 176 s it pairs the orchid with the complete Taipei 101 silhouette. Retain the 120–141 s observatory pass as a close landmark check. |
| Film, 150–195 s | At 168 and 176 s the audience sees Madame N's sanctuary on either side of the 175 s ecology reveal. Preserve these frames for courtyard lighting and director regressions. |
| Film, 275–360 s | The synthesis orbit yields to the care reveal from 335.5 s. At 342 s the eye is `(-0.58, 3.88, 4.38)` and looks toward `(-0.06, 3.84, 6.38)`. The ten-second slider pullback frames the orchid, vanity and tower through a clear window region at X ≈ 0.54–1.48, Y ≈ 3.58–5.71, Z ≈ 6.58. Protect that sightline during the morph and late hold. |

The renderer moves and rescales the Taipei kit during the finale. Its usual transform is 0.080 world units per metre, with the tower at X = 18.5, Z = 60. In the dawn alcove it settles at 0.064 with translation `(33, -9.6, 66)`. Convert camera positions to kit metres before selecting distance LODs. The listed coordinates describe the existing set. A future geometry gate must also sweep the actual camera, near plane, aircraft bounds and sightlines through the intervening frames, including the 1.8-second film cut blends. [Kit geometry](../tools/gen_trilogy_meshes.py) · [Camera paths](../src/render/m1_scene.cpp) · [Kit transforms](../src/render/trilogy_renderer.cpp)

## Mesh and shader contracts at the baseline

`BMESH001` uses little-endian fields. The current loader caps a file at 256 MiB, 2,000,000 vertices, 6,000,000 indices and 1,024 submeshes. Keep existing assets readable. An independently versioned placement table can refer to version-1 module geometry. If geometry layout itself changes, add an explicit versioned reader and fixture. [Exporter](../tools/gen_trilogy_meshes.py) · [Loader](../src/render/bmesh.cpp) · [CPU vertex and submesh types](../include/boo/bmesh.hpp)

| Section | Byte layout |
| --- | --- |
| Header, 64 bytes | 0–7 ASCII magic. Six uint32 at 8, 12, 16, 20, 24, 28 give version 1, header bytes 64, vertex stride 40, vertex/index/submesh counts. At 32–55 are float32 AABB min XYZ and max XYZ. At 56 and 60 are payload CRC32 and byte count. |
| Submesh table | 24 bytes each. Six uint32 values in order give first index, index count, material, part, first vertex, vertex count. |
| Vertices | 40 bytes each. Position float32×3 at 0, normal SNORM16×4 at 12, tangent SNORM16×4 at 20 (W gives handedness), UV/thickness/roughness float16×4 at 28, material and part uint16 at 36 and 38. Runtime expands to four float4s (64 bytes). |
| Indices | uint32 triangle-list indices in mesh-wide vertex space. The runtime rebases these when concatenating kits. |

The main mesh root signature serves both SM 6.0 rasterization and SM 6.5 inline DXR. All entries below use register space 0. [Root construction and bindings](../src/render/trilogy_renderer.cpp) · [Shader declarations and ray hits](../assets/shaders/m2_mesh.hlsl)

| Root index | Binding |
| --- | --- |
| 0, 1 | `b0` scene (36 DWORDs), `b1` draw (16 DWORDs, three transform rows plus first index, material, packed part and motion data) |
| 2, 3, 4 | Root SRVs `t0` vertices, `t1` indices, `t2` TLAS |
| 5 | SRV table at `t3…t7` containing the Tensor field, albedo/alpha, normal, ORMT and environment arrays |
| 6 | Root UAV `u0` for ray verification counters |
| Static samplers | `s0` linear clamp, `s1` anisotropic wrap, `s2` equirectangular |

The signature costs 61 of 64 DWORDs. The first unassigned register numbers in space 0 are `t8`, `b2`, `u1` and `s3`. A register still needs a root binding and descriptor storage. Extending root table 5 can accommodate extra SRVs without increasing root DWORD use. The current shader-visible heap reserves indices 0–4 for static SRVs and begins four 20-descriptor frame regions at index 5. Move that base or allocate another explicitly bounded range when adding descriptors. Space 1 is already used by separate accumulation (`t0`), resolve/unpack (`b0`, `t0…t3`) and NRD pack (`b0`, `t0…t3`, `u0…u3`) signatures. [Renderer](../src/render/trilogy_renderer.cpp) · [Microsoft D3D12 root signature limits](https://learn.microsoft.com/en-us/windows/win32/direct3d12/root-signature-limits)

## Implementation and evidence by phase

| PR phase | Implementation and review gate |
| --- | --- |
| 0 · Baseline | Capture clean, fixed-sample RGB10 frames using deterministic director inputs. Record the cue, sample, preset, internal resolution, adapter, enabled paths, timing sample count, GPU p50/p95 and peak process-local VRAM. Compare like for like. Repeat the baseline after any outstanding preview changes are integrated. |
| 1 · Instancing | Batch by reusable mesh, material and pass. Represent each transform explicitly as three float4 rows compatible with DXR's 3×4 descriptor and test translation, rotation and non-uniform scaling through raster and ray hits. Two transforms and four uint32 fields occupy 112 bytes. Index mesh metadata for index ranges and preserve motion-vector packing, masks and dynamic marbles. Today both candidate and committed ray queries interpret `InstanceID` as the first index. Convert **every** hit lookup to instance → geometry → index, including alpha cutouts. Bound the 24-bit ID and fence per-frame uploads. Record visible module count against the 5× target, draw calls and TLAS/BLAS bytes. Give rays access to offscreen shadow casters and reflected buildings. Use position-based LOD fades for direct-seek repeatability. |
| 2 · Taipei kits | Build qilou, ceramic walk-ups, historic shophouses and contemporary curtain walls as reusable modules. Reserve Dihua-style gables for their proper setting or an explicitly fictional block. Preserve the tower and orchid geometry and test near-plane and hero visibility on the actual shots above. |
| 3 · Interiors | Bake 6–8 original seeded room layouts to a mipmapped atlas array. Export local facade UVs and tangents from module space. Select rooms by stable instance/floor/bay IDs and share occupancy, tint and audio response across primary and secondary shading. Fade interiors by projected window size and distance before the 350 m geometry switch. Compare 1:1 crops plus motion for shimmer and measure the pass delta against 1.2 ms at 1440p. |
| 4 · Taipei vertical slice | Tune silhouettes and spacing before neon intensity. Compare near, middle and far detail during the opening descent and the late trilogy wide. Provide a matched 10–15 s before/after clip, a real 8 GB fallback run, 1440p GPU p95 ≤ 12 ms and a live clean Spout receiver check. Measure presented frame pacing separately from GPU work. |
| 5 · Other shots | Extend Shanghai's bridge and riverbanks in the trilogy, Shenzhen's waterfront in both cues, and the full film's architectural-to-enamel transition. Keep the 342 s reveal and tower-window sightline intact. Check all seven scenes in both editions, seek/wrap, intact audio timing and a sustained 1440p Showcase playthrough. Measure the separate 3840×2160 `4k_experimental` preset against its own pre-change timings. |
| 6 · Distribution | Keep offline geography provenance with any shipped derivative. Verify the staged assets, installed MSIX, localized resources, clean-install/update behavior and Windows App Certification Kit results before the Store release. |

For the qilou kit, Taipei Article 7 gives 3.64 m arcade width, at least 3.52 m wall setback, 3.33 m under the front beam and 2.50 m clear passage where columns exist. The 0.15 m dimension sets the column face back from the building line. The floor meets the adjoining sidewalk. Where there is no sidewalk, the outer edge rises 0.10–0.20 m above the road edge. Local planning and building technical rules can supersede these defaults. [Taipei City Building Management Ordinance, Article 7](https://laws.gov.taipei/law/LawSearch/LawArticleContent/FL004106)

Geographic accuracy and facade style need separate evidence. Compare the tower positions, mapped footprints, known heights and terrain from a fixed viewpoint. Mark inferred heights in the input data. The baseline uses procedural city geometry. The separately developed Xinyi footprint prototype uses OpenStreetMap. If that prototype is selected, ship its attribution and applicable derived data or change set alongside the work, and keep that provenance distinct from the CC0 facade kit. Credit identifiable source works and their authors as appropriate. Verify rights for each imported model and any source textures before it enters the package. [OSMF produced-work guidance](https://osmfoundation.org/wiki/Licence/Community_Guidelines/Produced_Work_-_Guideline) · [OSMF attribution guidance](https://osmfoundation.org/wiki/Licence/Attribution_Guidelines) · [Taiwan Copyright Act, Articles 58 and 64](https://law.moea.gov.tw/EngLawContent.aspx?id=10119&lan=E&media=downloadPDF)
