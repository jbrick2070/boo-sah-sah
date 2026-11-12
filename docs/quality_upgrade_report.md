# Film quality and robustness verification

Verified locally on 2026-11-11, starting from `4533892` on branch `codex/film-quality-robustness`. Host: Windows 11, NVIDIA GeForce RTX 5080 Laptop GPU (16 GB, CUDA driver API 13.5), Intel AI Boost NPU, MSVC C++20 `/W4 /WX`. The original executable, shaders, assets, captures and timing receipts are retained under `build/quality_baseline/`.

The implementation and functional gates pass in Release and Debug. **The zero D3D12 debug-layer warnings/errors acceptance item remains unverified:** Windows Graphics Tools is absent on this host. Receipts record `null` counts and `warning_validation: unavailable`; they do not report a clean debug run. Installing that Windows component requires elevation here. Re-run capture and stress commands with `--require-debug-layer` on a machine with it installed.

## Visible changes

- Three deterministic mesh assets replace the generic second-half kits: sculpted enamel ribs and titanium arches, planted celadon terrace vessels and botanical iris structures, and a dawn alcove with an oval mirror, ceramic basin, oak vanity, plant and toothbrush. All seven `.bmesh` assets reproduced byte-for-byte across two Blender generations and passed CRC32, SHA-256, bounds, normals, tangents and nondegenerate-face checks. The three new meshes have 30,040 / 39,072 / 15,076 triangles respectively; all reported zero nonmanifold edges.
- Glass uses thickness-dependent absorption, restrained environment dispersion and shaded secondary hits. Rough reflections filter facade detail; matte surfaces use broad environment lighting instead of sharp chrome-like geometry reflections. Early depth testing skips hidden pixel work before ray queries and verification atomics. Tensor ribbons use the edition's jade/orchid palette, and a stable one-in-five subset of the full CUDA simulation is drawn during the mutiny to preserve the hero silhouette. Simulation workload and hardware verification remain intact.
- Presentation text uses 60 first-party capsule-SDF glyphs, with `台北 / TAIPEI` and Chinese numerals paired with Roman act numbers. The title/act motifs move to the right below the benchmark card when full telemetry is visible. Their shading plate stays clear of both statistics panels. Clean Cinema retains the left-aligned film composition. No third-party font binary is shipped.
- The director's five bounded controls reach geometry/iris motion, palette, light, bloom and a restrained cut accent. Deterministic captures use the sample-derived ScriptedDirector; live OpenVINO NPU execution is verified separately.

All original and updated contact sheets were visually inspected, including both editions, all four solos, raster pairs and narrative frames. Native 1440p title and full-telemetry layout captures were also inspected. The clean texture stays free of titles, captions, HUD, camera badges and A/B splits.

## Functional evidence

| Gate | Release | Debug |
|---|---|---|
| C++/HLSL build | Pass | Pass |
| Unit suite | 18/18 | 18/18 |
| Existing hardware integration + new quarantine test | 17/17 | 17/17 |
| Cue/manifest validation, 21 SHA-256 entries | Pass | Pass |
| Zero optional SDK static-import audit | Pass | Pass |
| Application smoke, fallback, letterbox, M1/M2 compatibility, solo and Spout | Pass | Pass |
| Trilogy native 1440p captures/determinism | Pass | Pass |
| Film captures | 68 narrative/clean PNGs + 2 stats-layout PNGs | Same |
| Film stress | 224/224 | 224/224 |
| App `--inject-device-lost`, with Spout enabled | One recovery, sample preserved | One recovery, sample preserved |
| LZMS install/upgrade/rollback/uninstall verification | Pass, NPU sidecar included | Pass, sidecar excluded |
| GUI worker responsiveness and Cancel | Pass | Pass |
| D3D12 debug-layer warnings/errors | Unavailable | Unavailable |

The 68 deterministic image files match byte-for-byte between configurations. Stats-layout images contain live timings and are judged visually instead. Captures cover 18, 36, 95, 145, 175, 225, **260**, 275, 318, 342 and 355 seconds in both editions, plus both editions of the RT/CUDA/Tensor/NPU solos and raster pairs. The 260-second sample explicitly covers Act VI. Packed RGB10A2 repeat-seek equality, FP16 HDR finiteness, nonblack output, PNG decode round trips, verified RT attribution and clean-feed isolation are checked by the gate.

Each stress run performs 28 synthetic MP4 ingest start/stop cycles, 56 Spout start/stop cycles, 63 swapchain resizes, 224 cross-act seeks, edition/preset/engine switches, odd dimensions, minimized size, fullscreen transitions and simulated DPI messages. Both runs checked 121,856 stereo audio frames at full gain, with maximum adjacent sample step **0.00213893**, below 0.010. No nonfinite HDR components or unintended black frames were detected.

| Stress memory peak after first 32 warm-up iterations | Release | Debug |
|---|---:|---:|
| Local VRAM warm / later | 438.4 / 438.4 MiB | 438.4 / 438.4 MiB |
| Private bytes warm / later | 1233.0 / 1280.3 MiB | 1236.5 / 1269.9 MiB |
| Working set warm / later | 732.0 / 746.2 MiB | 733.7 / 750.3 MiB |
| Process handles warm / later | 1625 / 1628 | 1620 / 1621 |
| Synthetic loss/recreate | 1.26 s | 1.68 s |

The stress gate enforces growth ceilings of 256 MiB local VRAM, 192 MiB each for private bytes/working set, and its configured handle budget. These finite runs show bounded growth, not proof against every long-duration driver leak.

A separate lifecycle regression blocks a WARP queue, quarantines its context, and verifies teardown waits for delayed fence release. Another forces a real five-second Spout publication timeout and verifies an independent receiver remains excluded until the publication retires. Teardown drains a quarantined queue without making its allocators reusable; if the bounded drain fails, it explicitly removes that D3D12 device before releasing resources.

The stress gate found and drove fixes for late-film audio phase discontinuities and stale NPU status after disabling acceleration. Frequency modulation now integrates phase analytically, edition changes crossfade fixed-frequency voices, and NPU disable publishes its status synchronously.

## GPU timings before and after

Values are D3D12 GPU timestamp **median / p95 in milliseconds**, from three-second playback windows beginning at each listed time. Both versions used Family edition, the same 2560×1440 presentation window, default HUD/captions, muted WASAPI audio, and the local NPU runtime. `balanced` renders a native 1920×1080 clean canvas; `showcase` renders 2560×1440. No GPU validation job ran concurrently. Startup compilation precedes the measured playback window; initial GPU frames are included. These are short spot measurements, not a thermally controlled full-film or sustained 60-second benchmark.

| Start (s) | 1080p before | 1080p after | 1440p before | 1440p after |
|---:|---:|---:|---:|---:|
| 36 | 4.91 / 5.06 | 2.66 / 3.02 | 6.23 / 6.59 | 3.09 / 3.36 |
| 95 | 3.30 / 3.82 | 5.06 / 5.44 | 5.84 / 6.25 | 7.09 / 8.19 |
| 175 | 5.00 / 5.15 | 3.58 / 3.90 | 8.45 / 8.69 | 4.23 / 4.60 |
| 225 | 4.03 / 4.55 | 2.76 / 3.11 | 8.33 / 8.55 | 3.58 / 3.90 |
| 275 | 5.01 / 5.32 | 7.74 / 8.86 | 8.65 / 9.19 | 9.97 / 10.98 |
| 318 | 4.37 / 4.95 | 7.82 / 8.93 | 8.22 / 8.41 | 11.14 / 11.77 |
| 342 | 4.73 / 4.99 | 2.67 / 3.03 | 7.29 / 7.45 | 3.22 / 3.53 |
| 355 | 2.98 / 3.20 | 2.91 / 3.25 | 5.25 / 5.37 | 3.63 / 3.97 |

Machine-readable measurements: [quality_timings.json](quality_timings.json).

The combined 275/318-second shots and the 95-second corridor cost more than the original simpler geometry. Other sampled shots became faster after removing hidden-pixel ray work and low-value sharp reflections. All measured final p95 values remain below the 16.67 ms GPU budget on this host; this does not establish that budget on lower-tier GPUs or include presentation/audio/driver scheduling costs.

Raw before/after receipts: `build/quality_baseline/timings/`, `build/quality_after/timings/`, and `build/quality_after/timing_comparison.json`.

## Installer and reproduction

`build/dist/BooSahSah-Setup-win64.exe` remains one native per-user LZMS installer containing one application executable. Both app and installer embed the procedural icon. GUI decompression/extraction runs on a worker; the progress bar has an indeterminate decompress phase followed by per-file progress. Cancel is cooperative at checkpoints around the indivisible Windows LZMS call, and rollback completes before reporting cancellation.

Packaging verifies isolated installation with `PATH=C:\Windows\System32`, paths containing spaces/`%`/`&`/`!`, config-preserving upgrades, explicit `--replace-config`, locked-file upgrade rollback, installed cue validation, locked-file uninstall failure and retry, and preservation of untracked user files. Uninstall returns failure synchronously for locked tracked content; only its own executable cleanup is deferred. The GUI probe observed worker progress, received a WM_NULL response in under 1 ms, cancelled with exit 1223, and found no installed files or transaction directories in either configuration.

A five-second installed Release playback with the same System32-only PATH verified all five engines (`RT`, `CUDA`, `Tensor`, `NPU`, `Codec`) as actually executed and Accelerated. It presented and published 602 Spout frames with zero effect faults, then uninstalled successfully. Evidence: `build/quality_installed_live.json` and `.log`. The strict debug-layer probe rejected this host as expected (`build/quality_strict_debug_probe.log`).

```powershell
./scripts/run_smoke.ps1 -Preset x64-base-release
./scripts/run_smoke.ps1 -Preset x64-base-debug -SkipBenchmarks
./build/x64-release/boo_integration_tests.exe --trilogy-captures --no-activate
./build/x64-release/boo_integration_tests.exe --film-captures --no-activate --capture-dir build/film_captures
./build/x64-debug/boo_integration_tests.exe --film-captures --no-activate --capture-dir build/film_captures_debug
./build/x64-release/boo_integration_tests.exe --film-stress --no-activate --capture-dir build/quality_stress_release
./build/x64-debug/boo_integration_tests.exe --film-stress --no-activate --capture-dir build/quality_stress_debug
./build/x64-release/boo_integration_tests.exe --lifecycle-tests --no-activate
./scripts/package_release.ps1
./scripts/test_installer_gui.ps1 -SetupPath build/dist/BooSahSah-Setup-win64.exe
./.venv/Scripts/python.exe tools/refresh_asset_hashes.py --check
./.venv/Scripts/python.exe tools/verify_trilogy_meshes.py
```

Local receipts are `build/quality_*.log`, `build/film_captures*/manifest.json`, `build/quality_stress_*/stress.json`, and the build directories' `diagnostics/smoke_device_recovery.json`. The Windows CI matrix is authored for CPU/build/import/package validation on standard runners; no remote GitHub Actions run is claimed by this local report.

## Remaining limits

- Glass is a single geometry refraction/reflection approximation with environment dispersion and Beer absorption, not spectral path tracing or volumetric multiple scattering. Rough reflections are filtered single hits, not stochastic multi-sample reflections. Short contact-shadow rays can still reveal faceting on close geometry.
- Raster uses the same triangles, exposure and camera composition, with environment substitutes for geometry reflections/refraction. Petals therefore look more opaque and mirror contents differ. The environment remains a stylized procedural city; this is not photorealistic architectural reconstruction.
- The glyph set covers only the authored title/act motifs and Latin UI, not general Traditional Chinese text.
- `--inject-device-lost` is a safely drained synthetic fault, not a physical TDR. GPU queue/fence waits are bounded; native CUDA/MFT/OpenVINO calls and cooperative worker joins cannot be guaranteed to finish under every driver failure. Actual monitor hotplug/DPI migration, physical camera behavior and OBS/Resolume interoperability were not newly exercised; the suite uses simulated DPI, synthetic media and an actual D3D11 shared-texture receiver.
- Zero debug-layer warnings/errors still needs the strict rerun described above. No claim of complete acceptance substitutes for that missing hardware-tool validation.

Implementation references: [Microsoft early depth testing](https://learn.microsoft.com/en-us/windows/win32/direct3dhlsl/sm5-attributes-earlydepthstencil), [Microsoft device removal semantics](https://learn.microsoft.com/en-us/windows/win32/api/d3d12/nf-d3d12-id3d12device5-removedevice).
