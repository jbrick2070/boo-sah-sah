# Taipei street architecture

The left column shows Phase 1 and the right column shows the 41 new street bays and varied roof profiles. The tower and orchid retain their authored geometry. The outer buildings around the two fixed views use the distant tier. The new streets also enter the close and middle tiers as the camera moves. [Matched frames](taipei.jpg)

All nine Release runs used the RTX 5080 Laptop GPU at 2560 × 1440 Showcase. Each timing window contains 60 frames after 24 warm-up frames. The integration suite required deterministic 10-bit replay and live hardware ray-tracing work.

| Scene and time | Before p50 / p95 (ms) | After p50 / p95 (ms) | Before / after VRAM (MiB) | Visible near / middle / far placements |
| --- | ---: | ---: | ---: | ---: |
| film taipei 8s | 5.43 / 5.60 | 5.40 / 5.58 | 1,479.8 / 1,482.6 | 1 / 1 / 267 |
| film sanctuary 168s | 7.83 / 7.89 | 7.87 / 8.32 | 1,480.6 / 1,483.4 | 1 / 23 / 355 |
| film sanctuary 176s | 8.94 / 9.06 | 8.94 / 9.51 | 1,480.6 / 1,483.4 | 1 / 25 / 390 |
| film shenzhen 258s | 6.37 / 6.53 | 6.46 / 6.88 | 1,499.1 / 1,501.9 | 1 / 0 / 397 |
| film orchid finale 318s | 8.51 / 8.61 | 8.51 / 9.01 | 1,499.1 / 1,501.9 | 1 / 25 / 388 |
| film dawn 352s | 8.24 / 8.32 | 8.24 / 8.68 | 1,499.1 / 1,501.9 | 1 / 0 / 325 |
| trilogy shanghai 15s | 5.90 / 5.92 | 5.91 / 6.29 | 1,410.9 / 1,413.8 | 0 / 0 / 0 |
| trilogy taipei 168s | 5.94 / 5.98 | 5.98 / 6.41 | 1,410.9 / 1,413.8 | 1 / 22 / 353 |
| trilogy taipei 176s | 4.25 / 5.49 | 4.17 / 5.55 | 1,410.9 / 1,413.8 | 1 / 11 / 374 |

[All timings](telemetry.csv) · [Individual GPU samples](samples.csv) · [Capture settings, hashes, geometry and acceleration states](manifest.json) · [Phase 1 source measurements](../city-phase1/README.md)
