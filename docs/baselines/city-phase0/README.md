# Phase 0 city reference

Nine clean frames from the scene assets and camera paths in `5ff4665`, captured on November 19, 2026. The PNGs and native 10-bit pixel hashes matched across two independent executions. [Frame sheet](frames.jpg) · [Capture settings, sample IDs and backend states](manifest.json) · [Shot summaries](telemetry.csv) · [All 540 measured frames](samples.csv)

| Cue and playhead | GPU p50 (ms) | GPU p95 (ms) | Peak local VRAM (MiB) |
| --- | ---: | ---: | ---: |
| [Taipei · film · 0:08](film_taipei_8s.png) | 4.41 | 4.86 | 1,491.8 |
| [Shanghai · trilogy · 0:15](trilogy_shanghai_15s.png) | 5.79 | 6.18 | 1,408.9 |
| [Shenzhen · film · 4:18](film_shenzhen_258s.png) | 6.06 | 6.57 | 1,510.4 |
| [Taipei · trilogy · 2:48](trilogy_taipei_168s.png) | 5.56 | 6.00 | 1,409.5 |
| [Taipei · trilogy · 2:56](trilogy_taipei_176s.png) | 4.24 | 5.34 | 1,409.5 |
| [NPU sanctuary · film · 2:48](film_sanctuary_168s.png) | 7.24 | 8.01 | 1,491.8 |
| [NPU sanctuary · film · 2:56](film_sanctuary_176s.png) | 8.36 | 9.45 | 1,491.8 |
| [Orchid finale · film · 5:18](film_orchid_finale_318s.png) | 7.88 | 8.38 | 1,510.4 |
| [Dawn · film · 5:52](film_dawn_352s.png) | 7.95 | 9.58 | 1,510.4 |

## Reproduce the comparison

Build Release and run from the repository root in PowerShell.

```powershell
./scripts/build.ps1
./build/x64-release/boo_integration_tests.exe --city-baseline --no-activate --capture-dir build/city-baseline
```

The reference used Windows 11 25H2 build 26200.9974, NVIDIA driver 621.14, an RTX 5080 Laptop GPU and the 2560×1440 Showcase preset. A hidden swapchain ran the normal renderer with the Family edition, deterministic scripted lighting and analytical audio. After 24 warm-up frames, each row measures 60 consecutive frames advancing by 800 samples at 48 kHz. Percentiles use nearest rank. DXGI local memory is sampled at frame retirement.

GPU timestamps cover the D3D12 direct queue from the clear through the presentation draw. Display wait, independent compute queues, CPU time and PNG readback fall outside that interval. The test compares native RGB10A2 after an intervening seek and writes lossless PNGs of the 8-bit conversion. These short windows measure the starting cost of each shot. Sustained frame pacing, 4K and physical 8 GB hardware have separate gates in the [architecture plan](../../city_architecture_upgrade.md).
