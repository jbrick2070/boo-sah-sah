# Licenses & Asset Provenance Manifest

## 1. First-Party Source Code & Procedural Assets

Jeffrey A Brick authored the film, soundtrack, original project code and procedural asset generators. The packed scenes also contain the Poly Haven contributions listed below. The M2 shader incorporates the RTXDI reservoir code identified in section 3.

- **No third-party font binaries**, commercial music recordings, copyrighted video clips, or scraped external model checkpoints are bundled or distributed.
- **Asset integrity.** Both `boo_sah_sah.exe --validate-cues` and `boo_cue_validate.exe` verify the bytes of all registered runtime files before validating the cue sheets. In a source checkout, `python tools/refresh_asset_hashes.py --check` also checks the registered tools and reports omissions.

| Asset Family | Paths | Provenance | License |
|---|---|---|---|
| Runtime & Tools C++20 / HLSL | `src/`, `include/`, `tools/`, `assets/shaders/` | Original first-party project code | MIT (`LICENSE`). The adapted RTXDI portion of `m2_mesh.hlsl` retains the NVIDIA RTX SDKs license (section 3). |
| Cue Schedules & Presets | `assets/cues/*.json`, `config/*` | Original first-party timeline & quality specifications | `config/*`: MIT. `assets/cues/*.json`: CC BY-NC 4.0 (`LICENSE-ASSETS.md`) |
| Packed 3D Meshes (`.bmesh`) | `assets/meshes/*.bmesh` | Original geometry and CC0 scans. The included Xinyi map extract has separate provenance below | CC0-1.0 for original geometry and scans. The Taipei map database has its own terms. |
| Packed PBR & IBL Textures (`.btex`) | `assets/textures/*.btex` | Generated deterministically by `tools/gen_pbr_textures.py` (authored + CC0 scans) | CC0-1.0 |
| Presentation Vector Glyphs | `tools/gen_title_glyphs.py`, generated table in `assets/shaders/m1_taipei_slice.hlsl` | 60 original capsule-SDF glyphs: Latin, digits/punctuation, 台北, and 一–七; no third-party font source | MIT (`LICENSE`) |
| Original 4-Track Soundtrack (OST) | `assets/audio/01_neon_orchid.flac` .. `04_glass_dragon.flac` | Original soundtrack by Jeffrey A Brick (*Neon Orchid*, *Acid Lotus*, *Velvet Cipher*, *Glass Dragon*) | CC BY-NC 4.0 (`LICENSE-ASSETS.md`) |
| Madame N Stage-Director IR | `assets/models/madame_n_director.{xml,bin,json}` | Generated deterministically by `tools/gen_madame_n_model.py` | MIT (`LICENSE`) |
| Poly Haven Scanned PBR, Botanical & HDRI Sources | `third_party/polyhaven_cc0/` (`provenance.json`) | Public-domain CC0-1.0 scans & HDRI compiled into `.bmesh` / `.btex` | CC0-1.0 |

### Taipei geographic sources

C.Y. Lee & Partners (李祖原聯合建築師事務所) designed Taipei 101. BOO SAH SAH is an independent production. [Architect’s project page](https://www.cylee.com/project/Taipei-101)

Taipei map data © OpenStreetMap contributors, licensed under the Open Database License 1.0. The release includes the machine-readable geographic extract and its notice in `map-data/`. [OpenStreetMap copyright and licensing](https://www.openstreetmap.org/copyright)

The offline Taipei relief generator uses global SRTM terrain data courtesy of the U.S. Geological Survey, distributed through Mapzen Terrain Tiles. The project sampled the N24E121 and N25E121 tiles on a coarser grid around Taipei 101 and flattened low urban surface returns beneath the building geometry. [Mapzen Terrain Tiles](https://registry.opendata.aws/terrain-tiles/) · [Terrain data attribution](https://github.com/tilezen/joerd/blob/master/docs/attribution.md)

The seven distributed mesh assets are `shanghai_trestle`, `shenzhen_evtol`, `taipei_101_hero`, `glass_orchid_hero`, `enamel_cathedral`, `microbial_sanctuary`, and `dawn_alcove`. The four distributed GPU texture-array assets are `pbr_albedo_alpha.btex` (`BC7_UNORM_SRGB`), `pbr_normal.btex` (`BC5_UNORM`), `pbr_ormt.btex` (`BC7_UNORM`), and `pbr_ibl_env.btex` (`RGBA16F`). The limited Chinese title glyphs do not constitute a general Chinese font.

### Manifest fields

Schema 2 records a stable `id`, repository-relative `path`, `kind`, human-readable `provenance`, HTTPS `source_url`, `author`, `license`, `attribution_required`, and the lowercase SHA-256 digest of the exact file bytes. For generated artifacts, the source URL identifies the generator. Packed works that include external material also list its `upstream_sources`. The Poly Haven receipt below records each input file's URL and digest. License expressions describe the packaged artifact, including third-party portions. The attribution flag includes the MIT copyright and permission notice.

The `assets` array describes release files. The `source_tools` array covers the repository's Python and C++ tool sources. To update a digest after an intentional change, review the affected sources and license, run `python tools/refresh_asset_hashes.py`, and inspect the manifest diff.

### Bundled Public-Domain (`CC0-1.0`) Source Assets (`third_party/polyhaven_cc0/provenance.json`)

Every external scan, botanical 3D asset, and HDRI sky incorporated into `assets/meshes/*.bmesh` and `assets/textures/*.btex` is dedicated to the public domain under **Creative Commons Zero v1.0 Universal (`CC0-1.0`, `https://creativecommons.org/publicdomain/zero/1.0/`)** with unrestricted commercial and binary redistribution rights (`attribution_required: false`). Full per-file URLs and SHA-256 checksums are recorded in `third_party/polyhaven_cc0/provenance.json`:

1. **Scanned 1k PBR Material Sets (`CC0-1.0`, Poly Haven):**
   - `white_oak_veneer` (`https://polyhaven.com/a/white_oak_veneer`, Jenelle van Heerden)
   - `american_walnut_veneer` (`https://polyhaven.com/a/american_walnut_veneer`, Jenelle van Heerden)
   - `plastered_wall_02` (`https://polyhaven.com/a/plastered_wall_02`, Charlotte Baglioni)
   - `clay_plaster` (`https://polyhaven.com/a/clay_plaster`, Amal Kumar)
   - `long_white_tiles` (`https://polyhaven.com/a/long_white_tiles`, Jenelle van Heerden & Sergej Majboroda)
   - `terrazzo_tiles` (`https://polyhaven.com/a/terrazzo_tiles`, Amal Kumar)
   - `marble_01` (`https://polyhaven.com/a/marble_01`, Rob Tuytel)
   - `rough_linen` (`https://polyhaven.com/a/rough_linen`, Rico Cilliers & colormass)
   - `granite_tile_02` (`https://polyhaven.com/a/granite_tile_02`, Charlotte Baglioni)
   - `concrete_pavers_02` (`https://polyhaven.com/a/concrete_pavers_02`, Amal Kumar)
   - `asphalt_02` (`https://polyhaven.com/a/asphalt_02`, Rob Tuytel)
   - `concrete_moss` (`https://polyhaven.com/a/concrete_moss`, Rob Tuytel)
2. **Scanned Botanical glTF 3D Models & Alpha/PBR Leaf Maps (`CC0-1.0`, Poly Haven):**
   - `calathea_orbifolia_01` (`https://polyhaven.com/a/calathea_orbifolia_01`, Rico Cilliers & Rob Tuytel)
   - `potted_plant_04` (`https://polyhaven.com/a/potted_plant_04`, James Ray Cock)
   - `fern_02` (`https://polyhaven.com/a/fern_02`, Rico Cilliers & Rob Tuytel)
3. **HDRI Dawn Sky Environment (`CC0-1.0`, Poly Haven):**
   - `qwantani_dawn_puresky` (`https://polyhaven.com/a/qwantani_dawn_puresky`, Greg Zaal & Jarod Guest)

---

## 2. Inbox Windows 11 System Libraries (`BASE_ONLY` Build)

The portable `boo_sah_sah.exe` binary links exclusively against standard Microsoft Windows 11 SDK (`10.0.26100.0+`) and MSVC C++20 runtime libraries:

The application and native installer statically link the MSVC runtime. Setup uses Windows' inbox `cabinet.dll` LZMS compression API; no third-party installer framework or decompressor is bundled.

- **Direct3D 12 & DXGI** (`d3d12.dll`, `dxgi.dll`, `dxc.exe`) — Swapchain presentation, SM 6.0 rasterization, compute shaders, and DXR 1.1/1.2 inline `RayQuery`.
- **Direct3D 11 Interop** (`d3d11.dll`, loaded dynamically via `LoadLibraryExW`) — Optional Spout2 shared `BGRA8` clean-feed texture creation.
- **Windows Audio Session API** (`ole32.dll`, `avrt.dll`, `mmdeviceapi`) — 48,000 Hz stereo float presentation-locked master audio clock and endpoint migration.
- **Windows Media Foundation** (`mfplat.dll`, `mf.dll`, plus dynamically loaded `mfreadwrite.dll`) — Optional hardware H.264 encode/decode round-trip and strictly opt-in local video/webcam ingest.
- **Windows Configuration Manager** (`cfgmgr32.dll`) — Non-blocking PnP `ComputeAccelerator` NPU device discovery.

---

## 3. Dynamically Probed Optional Driver & Runtime Interfaces

**NVIDIA RTX™ graphics.** The Windows hardware build uses the NVIDIA DLSS SDK for Super Resolution and Ray Reconstruction. NVIDIA Streamline integrates DLSS Frame Generation and NVIDIA Reflex. The following files accompany the production runtimes.

- `dlss/LICENSE-NVIDIA-DLSS.txt` covers the DLSS SDK.
- `streamline/license.txt` and `streamline/3rd-party-licenses.md` cover Streamline and its third-party components.
- `streamline/nvngx_dlss.license.txt` and `streamline/reflex.license.txt` accompany the feature plugins.

`boo_sah_sah.exe` carries **zero static link dependencies** on proprietary vendor toolkits (`dumpbin /dependents` verified):

1. **NVIDIA CUDA Driver API (`nvcuda.dll`)** — Installed by standard NVIDIA GeForce/Studio display drivers in `System32` (not bundled). Dynamically probed for GPU-resident PTX particle simulation (`M3`) and Tensor Core `mma.sync.m16n8k16` neural caustic evaluation (`M4`). Falls back automatically to D3D12 HLSL compute substitutes when absent.
2. **NVIDIA Video Codec Driver API (`nvEncodeAPI64.dll`) & Hardware H.264 MFTs** — Installed by GPU drivers (not bundled). Probed on demand when Signal Hijack (`M9`) or `[B]` benchmark sweep is active. Falls back automatically to `SHADER SIGNAL` macroblock quantization when absent or disabled.
3. **Intel OpenVINO C Runtime (`openvino_c.dll` + `openvino_intel_npu_plugin.dll`)** — Apache-2.0 (`https://github.com/openvinotoolkit/openvino`, license included at `openvino/LICENSE-OpenVINO-Apache-2.0.txt` when staged into `BooSahSah-Setup-win64.exe` via `scripts/package_release.ps1`, or omitted via `-ExcludeOpenVinoRuntime`). Dynamically probed for explicit `"NPU"` (or `"CPU"` fallback) execution of the Madame N stage-director MLP (`M5`). Falls back automatically without stalling playback to the deterministic `ScriptedDirector` when OpenVINO DLLs or Intel NPU hardware are absent.
4. **Spout2 Receiver Applications (OBS Spout2 Plugin / Resolume)** — Interoperates via standard `SpoutSenderNames` / `SpoutSharedTextureInfo` named shared memory and DXGI shared handles (`M8`) with zero third-party Spout SDK DLLs linked or shipped.
5. **NVIDIA Real-Time Denoisers (`nrd\NRD.dll`, v4.18.0) & RTXDI Reservoir Sampling (`RTXDI_DIReservoir`)** — Licensed under the **NVIDIA RTX SDKs License** (`https://github.com/NVIDIA-RTX/NRD`, `https://github.com/NVIDIA-RTX/RTXDI`; full license text bundled at `third_party/nvidia_nrd/LICENSE-NVIDIA-NRD.txt` and staged at `nrd/LICENSE-NVIDIA-NRD.txt` by `scripts/package_release.ps1` even when `-ExcludeNrdRuntime` omits the optional DLL). `NRD.dll` is compiled with embedded DXIL compute shaders (`NRD_EMBEDS_DXIL_SHADERS=ON`) and loaded exclusively at runtime via `LoadLibraryExW` + `GetProcAddress` (`src/render/nrd_bridge.cpp`) with zero static PE import dependencies on `boo_sah_sah.exe`, falling back automatically to the built-in D3D12 progressive HLSL accumulator when absent. *Attribution Notice:* **This software contains source code provided by NVIDIA Corporation.**
6. **Windows XInput Gamepad API (`xinput1_4.dll` / `xinput9_1_0.dll`)** — Inbox Windows library probed dynamically via `LoadLibraryExW` + `GetProcAddress` (`src/app/main.cpp`) with zero static PE import dependencies for optional 6-DOF Explore camera gamepad flight.
