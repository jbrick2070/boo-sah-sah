# Benchmark 2.2 plan: render test and film tour

Decided 2026-11-28 with the owner, after 2.1 went to certification and while 2.1.1 (the compact results window) is in progress. 2.2 adds two new benchmark choices; the existing fixed-shot frame-time tests (Raster, Path tracing, DLSS Quality) stay unchanged so older results remain comparable.

## 1. Render test (watch one hero frame build up)

- One hero frame: the orchid in the sanctuary at dusk, full resolution, path traced.
- Rendered in tiles that sweep across the screen. Each tile starts noisy and clears as samples accumulate. No flashing: the image only gets sharper.
- Live overlay: tiles done / total, samples per pixel, elapsed time.
- Score: total render time and samples per second, next to the RTX 5080 reference. The finished frame is shown full screen; "Save picture..." saves the full-quality render.
- GPUs without ray tracing render the same frame tile by tile in raster with high sample counts (supersampled), labelled as the raster version of the test, scored separately.
- Our own look and wording; not a copy of any existing benchmark's layout or branding.

## 2. Film tour (several scenes, measured one by one)

- 5-6 scenes, about 4 s each, joined by short crossfades (no hard cuts; well under 3 flashes per second): dusk opening, sanctuary stelae and orchid, the particle vortex from above, Shanghai river, orbit over Earth, dawn bathroom.
- Fixed camera paths and seeds so runs are repeatable.
- Results: a frame time for each scene plus an overall score; runs in the chosen mode (raster, path tracing or DLSS Quality).
- About 25 s per run.

## Shared work

- Results window (the 2.1.1 compact design) gains rows for the new tests and labels the test version.
- New RTX 5080 reference numbers for both tests; Iris Xe and RTX 4060 captures for QA.
- Strings in all 25 languages; Store notes and certification notes updated (the certification steps mention the benchmark).
- Photosensitivity check: crossfades only, no full-screen brightness jumps.
- Tests: deterministic tile order and scene schedule, cancellation mid-run, Lite/Safe presets, GPUs without ray tracing.
