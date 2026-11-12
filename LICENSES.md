# Licenses & Asset Provenance Manifest

## 1. First-Party Source Code & Procedural Assets

All C++20 source files (`src/`, `include/`, `tools/`, `tests/`), HLSL shaders (`assets/shaders/`), procedural bitmap glyph tables, timeline cue sheets (`assets/cues/`), procedural audio synthesizer arrangements (`src/audio/engine.cpp`), procedural `.bmesh` geometry (`assets/meshes/`, generated via `tools/gen_trilogy_meshes.py`), compact neural stage-director IR weights (`assets/models/madame_n_director.xml` / `.bin`, generated via `tools/gen_madame_n_model.py`), and configuration files (`config/`) in this repository were authored originally for **BOO SAH SAH — The Glass Orchid Protocol**.

- **No third-party font binaries**, commercial music recordings, copyrighted video clips, or scraped external model checkpoints are bundled or distributed.
- **Asset Integrity**: Every distributed shader, cue sheet, packed `.bmesh` mesh, and neural stage-director artifact is enumerated in `assets/manifest.json` with license metadata and SHA-256 verification checksums enforced by `boo_cue_validate.exe`.

| Asset Family | Paths | Provenance | License |
|---|---|---|---|
| Runtime & Tools C++20 / HLSL | `src/`, `include/`, `tools/`, `assets/shaders/` | Original first-party project code | Proprietary / Project First-Party |
| Cue Schedules & Presets | `assets/cues/*.json`, `config/*` | Original first-party timeline & quality specifications | Proprietary / Project First-Party |
| Packed 3D Meshes (`.bmesh`) | `assets/meshes/*.bmesh` | Generated deterministically by `tools/gen_trilogy_meshes.py` | CC0-1.0 |
| Madame N Stage-Director IR | `assets/models/madame_n_director.{xml,bin,json}` | Generated deterministically by `tools/gen_madame_n_model.py` | Proprietary / Project First-Party |

---

## 2. Inbox Windows 11 System Libraries (`BASE_ONLY` Build)

The portable `boo_sah_sah.exe` binary links exclusively against standard Microsoft Windows 11 SDK (`10.0.26100.0+`) and MSVC C++20 runtime libraries:

- **Direct3D 12 & DXGI** (`d3d12.dll`, `dxgi.dll`, `dxc.exe`) — Swapchain presentation, SM 6.0 rasterization, compute shaders, and DXR 1.1/1.2 inline `RayQuery`.
- **Direct3D 11 Interop** (`d3d11.dll`, loaded dynamically via `LoadLibraryExW`) — Optional Spout2 shared `BGRA8` clean-feed texture creation.
- **Windows Audio Session API** (`ole32.dll`, `avrt.dll`, `mmdeviceapi`) — 48,000 Hz stereo float presentation-locked master audio clock and endpoint migration.
- **Windows Media Foundation** (`mfplat.dll`, `mf.dll`, plus dynamically loaded `mfreadwrite.dll`) — Optional hardware H.264 encode/decode round-trip and strictly opt-in local video/webcam ingest.
- **Windows Configuration Manager** (`cfgmgr32.dll`) — Non-blocking PnP `ComputeAccelerator` NPU device discovery.

---

## 3. Dynamically Probed Optional Driver & Runtime Interfaces

`boo_sah_sah.exe` carries **zero static link dependencies** on proprietary vendor toolkits (`dumpbin /dependents` verified):

1. **NVIDIA CUDA Driver API (`nvcuda.dll`)** — Installed by standard NVIDIA GeForce/Studio display drivers in `System32` (not bundled). Dynamically probed for GPU-resident PTX particle simulation (`M3`) and Tensor Core `mma.sync.m16n8k16` neural caustic evaluation (`M4`). Falls back automatically to D3D12 HLSL compute substitutes when absent.
2. **NVIDIA Video Codec Driver API (`nvEncodeAPI64.dll`) & Hardware H.264 MFTs** — Installed by GPU drivers (not bundled). Probed on demand when Signal Hijack (`M9`) or `[B]` benchmark sweep is active. Falls back automatically to `SHADER SIGNAL` macroblock quantization when absent or disabled.
3. **Intel OpenVINO C Runtime (`openvino_c.dll` + `openvino_intel_npu_plugin.dll`)** — Apache-2.0 (`https://github.com/openvinotoolkit/openvino`, license included at `openvino/LICENSE-OpenVINO-Apache-2.0.txt` when staged into `BooSahSah-Setup-win64.exe` via `scripts/package_release.ps1`, or omitted via `-ExcludeOpenVinoRuntime`). Dynamically probed for explicit `"NPU"` (or `"CPU"` fallback) execution of the Madame N stage-director MLP (`M5`). Falls back automatically without stalling playback to the deterministic `ScriptedDirector` when OpenVINO DLLs or Intel NPU hardware are absent.
4. **Spout2 Receiver Applications (OBS Spout2 Plugin / Resolume)** — Interoperates via standard `SpoutSenderNames` / `SpoutSharedTextureInfo` named shared memory and DXGI shared handles (`M8`) with zero third-party Spout SDK DLLs linked or shipped.
