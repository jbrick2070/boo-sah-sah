# Waterfront and full-film graphics

Shanghai's railway bridge now has laced steel posts, tapered knee plates, overhead lattice work and alternating maintenance lamps. Across the river, columns and stepped galleries sit in front of the older façades, and six lower office blocks extend the second row of the skyline. The Shanghai source mesh grows from 43,812 to 51,844 triangles. Shenzhen’s waterfront towers gain stepped roof screens and open crowns that frame the sky between the two cores. The public aircraft and its camera follow the original path. [Matched waterfront frames](waterfronts.jpg)

## The seven film acts

| Film time | Scene | What to look for |
| --- | --- | --- |
| 0–45 s | Taipei opening | The ceramic street fronts and roof equipment from the Taipei slice carry into the opening skyline. [Taipei in motion](../city-phase4/README.md) |
| 45–100 s | Mirror Intelligence | Etched brass fans border the mirror frames. The bases of the adjacent enamel columns pick up jade tones. The ray-hit material uses the same finish when a frame appears in another reflection. |
| 100–150 s | Neural Velvet | Gold and teal threads cross the Tensor ribbons. The weave follows their surface coordinates while the neural field controls thread intensity. |
| 150–195 s | Madame N | Two colored spots project five-petal cutouts onto the set and illuminate the orchid midribs. The director’s palette, bloom and light values steer the fixtures. The CPU-scripted and NPU paths drive the same lights. |
| 195–245 s | Particle Mutiny | Cool and warm spiral streams reveal the particle flow around the orchid. A subset of motes stretches along its projected velocity. Reduced effects draw fewer motes and omit the trails. |
| 245–275 s | Signal Hijack | Shenzhen’s new rooftop profiles give the skyline behind the aircraft a more varied edge during the tracking shot. |
| 275–360 s | Silicon Rebellion | A jade-to-pearl pattern grows across the glass petals, then recedes before the camera reveals the orchid on the vanity. |

[Matched first-half frames](film-first-half.jpg) · [Matched second-half frames](film-second-half.jpg)

## RTX 5080 Laptop GPU

The paired public builds start from Phase 3 (`71f3e46`) and Phase 5 (`a5c7966`). Each uses the Family edition and 2560 × 1440 Showcase settings. Each fixed sample was measured over 60 GPU frames after 24 warm-up frames. VRAM records peak process-local video memory. The timing covers the direct queue through the final presentation draw, with display wait and capture readback outside its scope.

| Scene | Before GPU p50 / p95 (ms) | After GPU p50 / p95 (ms) | Before / after VRAM (MiB) |
| --- | ---: | ---: | ---: |
| Film · taipei 8s | 5.49 / 5.97 | 5.57 / 6.03 | 1483 / 1486 |
| Film · mirror 95s | 6.88 / 7.36 | 7.16 / 9.38 | 1484 / 1487 |
| Film · neural 145s | 5.89 / 6.37 | 6.12 / 6.63 | 1484 / 1487 |
| Film · sanctuary 168s | 8.22 / 10.53 | 7.16 / 9.21 | 1484 / 1487 |
| Film · sanctuary 176s | 9.06 / 9.57 | 7.95 / 8.53 | 1484 / 1487 |
| Film · particle 225s | 7.62 / 8.15 | 7.56 / 8.16 | 1484 / 1487 |
| Film · shenzhen 258s | 6.53 / 6.86 | 6.52 / 6.94 | 1503 / 1506 |
| Film · orchid finale 318s | 8.62 / 9.09 | 7.38 / 7.93 | 1503 / 1506 |
| Film · dawn 352s | 8.38 / 9.60 | 8.38 / 8.89 | 1503 / 1506 |
| Trilogy · shanghai 15s | 5.98 / 6.46 | 6.69 / 7.16 | 1415 / 1418 |
| Trilogy · taipei 168s | 6.06 / 6.51 | 6.22 / 6.67 | 1415 / 1418 |
| Trilogy · taipei 176s | 4.28 / 4.49 | 4.32 / 4.60 | 1415 / 1418 |

[Before capture settings and geometry](before-manifest.json) · [After capture settings and geometry](after-manifest.json) · [Before individual timing samples](before-samples.csv) · [After individual timing samples](after-samples.csv)

## Reproduce

Run the Release integration executable from its matching worktree so the harness loads the intended shaders and meshes. The optional 4K mode uses the separate 3840 × 2160 Experimental preset.

```powershell
.\scripts\build.ps1
.\build\x64-release\boo_integration_tests.exe --show-baseline --no-activate --capture-dir build\show-1440p
.\build\x64-release\boo_integration_tests.exe --show-baseline --baseline-4k --no-activate --capture-dir build\show-4k
```
