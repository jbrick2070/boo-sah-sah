# City instancing on the RTX 5080 Laptop GPU

Shenzhen gains stepped towers and grouped office windows behind the aircraft. In Taipei the additional rooflines and dark floors appear in the orchid reflections. The two rows use exactly the same camera and audio sample across each pair. [Before and after](comparison.jpg)

The left column uses the published Phase 0 reference. The right column uses this pull request with the same public meshes. [Earlier reference](../city-phase0/README.md)

## Frame times and memory

Windows 11, NVIDIA driver 621.14, 2560 × 1440 Showcase, Family edition. Each window covers 60 frames after 24 warm-up frames. Measurements cover GPU time on the D3D12 direct queue through the presentation draw. Display wait, external compute queues and capture readback fall outside the measurement. Memory shows peak DXGI process-local usage. Times are milliseconds and memory is MiB.

| Shot | Before p50 / p95 | After p50 / p95 | After raster p50 / p95 | VRAM before / after |
| --- | ---: | ---: | ---: | ---: |
| [Film · Taipei 0:08](film_taipei_8s.png) | 4.41 / 4.86 | 5.43 / 5.60 | 1.29 / 1.30 | 1,491.8 / 1,479.8 |
| [Film · sanctuary 2:48](film_sanctuary_168s.png) | 7.24 / 8.01 | 7.83 / 7.89 | 1.55 / 1.59 | 1,491.8 / 1,480.6 |
| [Film · sanctuary 2:56](film_sanctuary_176s.png) | 8.36 / 9.45 | 8.94 / 9.06 | 1.61 / 1.63 | 1,491.8 / 1,480.6 |
| [Film · Shenzhen 4:18](film_shenzhen_258s.png) | 6.06 / 6.57 | 6.37 / 6.53 | 1.34 / 1.35 | 1,510.4 / 1,499.1 |
| [Film · finale 5:18](film_orchid_finale_318s.png) | 7.88 / 8.38 | 8.51 / 8.61 | 1.70 / 1.71 | 1,510.4 / 1,499.1 |
| [Film · dawn 5:52](film_dawn_352s.png) | 7.95 / 9.58 | 8.24 / 8.32 | 1.53 / 1.54 | 1,510.4 / 1,499.1 |
| [Trilogy · Shanghai 0:15](trilogy_shanghai_15s.png) | 5.79 / 6.18 | 5.90 / 5.92 | 1.22 / 1.23 | 1,408.9 / 1,410.9 |
| [Trilogy · Taipei 2:48](trilogy_taipei_168s.png) | 5.56 / 6.00 | 5.94 / 5.98 | 1.40 / 1.41 | 1,409.5 / 1,410.9 |
| [Trilogy · Taipei 2:56](trilogy_taipei_176s.png) | 4.24 / 5.34 | 4.25 / 5.49 | 1.28 / 1.30 | 1,409.5 / 1,410.9 |

[Before timings](../city-phase0/telemetry.csv) · [After timings](telemetry.csv) · [All measured frames](samples.csv) · [Geometry and backend telemetry](manifest.json) · [Forced-raster receipts](raster/manifest.json)

## Geometry submission

| Shot | Old color instances | New color instances | Increase | Visible buildings | City modules / draw calls |
| --- | ---: | ---: | ---: | ---: | ---: |
| Film · Taipei 0:08 | 52 | 774 | 14.88× | 269 | 722 / 33 |
| Film · sanctuary 2:48 | 126 | 1047 | 8.31× | 351 | 921 / 30 |
| Film · sanctuary 2:56 | 126 | 1131 | 8.98× | 380 | 1005 / 30 |
| Film · Shenzhen 4:18 | 76 | 870 | 11.45× | 398 | 794 / 26 |
| Film · finale 5:18 | 174 | 1181 | 6.79× | 378 | 1007 / 30 |
| Film · dawn 5:52 | 79 | 929 | 11.76× | 322 | 850 / 30 |
| Trilogy · Shanghai 0:15 | 36 | 36 | 1.00× | 0 | 0 / 0 |
| Trilogy · Taipei 2:48 | 52 | 973 | 18.71× | 350 | 921 / 30 |
| Trilogy · Taipei 2:56 | 52 | 1017 | 19.56× | 364 | 965 / 30 |

The previous renderer issued one color instance for each active authored submesh. The old column counts those submeshes in the source kits selected by the original camera mode. The new column totals the per-view city modules and one instance per remaining non-city draw. These scripted shots contain no interactive marbles. A building can contribute multiple modules. The two hero views use the distant geometry tier. The CPU tests also cover close and middle geometry, bounds and transitions.

Every preferred-path frame verified live DXR work. Forced raster reported the substitution and zero ray instances on all nine frames. Both paths matched native 10-bit pixels on repeat seek at every anchor. Windows Graphics Tools was unavailable, so a strict debug-layer error count remains pending. Physical 8 GB validation also remains open.

## Reproduce

Run from the repository root after a Release build. Resolve the executable before setting the child working directory.

```powershell
$testExe = (Resolve-Path ./build/x64-release/boo_integration_tests.exe).Path
& $testExe --city-baseline --no-activate --capture-dir build/city-dxr
& $testExe --city-baseline-raster --no-activate --capture-dir build/city-raster
```
