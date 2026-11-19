# Taipei arcade windows

Six original room interiors give the same shop bays different back walls, lit fixtures, blinds and side walls. Each vertical pair shows the full camera shot and the same 760 × 110 native-pixel crop. [Matched front and oblique views](arcades.jpg)

The two cameras share a trilogy cue at 176 seconds. The release harness pins their pose, renders the same sample after a backward seek, and compares the 10-bit output. Each performance window contains 60 GPU samples after 24 warm-up frames at 2560 × 1440 on the RTX 5080 Laptop GPU. The timings cover the direct queue, excluding window presentation and external compute queues.

| Camera | Previous DXR p50 / p95 (ms) | Updated DXR p50 / p95 (ms) | Previous raster p50 / p95 (ms) | Updated raster p50 / p95 (ms) | New DXR peak local VRAM | Near / middle / far placements |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Front | 11.09 / 11.60 | 11.09 / 11.69 | 1.36 / 1.55 | 1.38 / 1.53 | 1408.9 MiB | 8 / 4 / 294 |
| Oblique | 9.88 / 10.48 | 9.97 / 10.61 | 1.36 / 1.39 | 1.37 / 1.53 | 1408.9 MiB | 9 / 9 / 317 |

The mipmapped array uses 655,384 bytes on disk. All five interior faces use local façade coordinates. The shader fades the room lookup between 160 and 250 metres and disables it on distant buildings.

All eight comparisons reproduced the exact 10-bit frame after seeking. The measured p95 cost of the new interiors was 0.09 ms head-on and 0.13 ms from the oblique angle on the ray-tracing path.

[Updated DXR manifest](new-manifest.json) · [Updated raster manifest](raster-manifest.json) · [Previous DXR manifest](previous-manifest.json) · [Previous raster manifest](previous-raster-manifest.json)
