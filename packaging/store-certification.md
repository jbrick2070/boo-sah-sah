# Microsoft Store: certification and submission answers

Paste each block into the matching Partner Center field. Limits are Partner Center's: notes for certification 6,000 characters (the field allows 6,000), restricted capability justification 500.

> The repository is public. The privacy policy address below works once `PRIVACY.md` is on `main` (PR "License and public privacy policy"). Check it in a signed-out browser window before submitting.

## Notes for certification (Submission options)

```
2.1 (replaces the cancelled 2.0 submission and includes it) FIXES both earlier failures. 10.1.2.10 (froze at launch, Dell Inspiron 5379): start-up compiles no longer block the window, and the loading screen shows each step. 10.1.4.4: on GPUs without double-precision math (such as Intel Iris Xe) earlier versions showed only circles instead of the city; it now draws. NEW: film updates and a benchmark (step 2). Any GPU steps down if frames run long. The first launch is the longest while the driver prepares shaders.

REQUIRED GRAPHICS: Intel Iris Xe, AMD Radeon Vega 8 or NVIDIA GTX 1050, or better (stated in System requirements). Older Intel HD/UHD graphics, such as the UHD 620 in the Dell Inspiron 5379, are below the minimum. On them the app is built to start in a reduced Lite mode (960x540; no ray tracing, NPU, CUDA or PhysX) with the window responding while shaders compile, but this was NOT tested on real UHD hardware and performance is not guaranteed. Measured on Intel Iris Xe in Lite with the dashboard on: 44-60 fps. Tested on Iris Xe, an RTX 4060 and an RTX 5080.

BOO SAH SAH is an interactive six-minute film rendered on the viewer's PC. No account, ads or purchases; it works offline in 25 languages.

1. Press F to explore: W A S D fly, Q/E down/up, right-drag to look, Y drops marbles. F returns.
2. Press Ctrl+B (or Run benchmark in Hardware Orchestra), choose Raster and run. The film pauses for a fixed shot; read the GPU frame times; Save picture and text writes to a folder you choose; Back to film resumes. The report form opens only after you confirm.
3. Path tracing, DLSS Quality, Render test (one frame rendered tile by tile, about 25 s) and Film tour (six scenes, about 30 s) are separate tests; unavailable choices say why.
4. Outside Explore, in Safe or higher (in Lite press Q once): Shift+Up / Down add or remove bird flocks; Shift+F changes formation.
5. Space pauses; Left / Right seek; N / P change acts; Z music; Tab panels; F11 full screen.

Hardware: Direct3D 12 with Shader Model 6.0. Ray tracing, NVIDIA and an NPU are optional; integrated and AMD graphics start lighter. Unsupported PCs get a plain message.

Camera: off by default; W (outside Explore) or a launch option turns it on, with a red LIVE CAMERA label; nothing is recorded, saved or sent. F12 saves a film frame (never the camera) to Pictures\BOO SAH SAH when installed from the Store. The app makes no network connection.
```

## Restricted capability: runFullTrust (justification, 500 characters at most)

```
BOO SAH SAH is a Win32 Direct3D 12 desktop app (C++). Full trust lets it use the GPU directly through D3D12 and DXGI, play audio through WASAPI, and load optional GPU helper libraries (NVIDIA denoiser, DLSS and Streamline, OpenVINO) from its own install folder at launch. For a requested film benchmark it starts its own executable in a local Windows job. This lets the viewer cancel an unresponsive graphics worker. Saving is local.
```

## Device capability: webcam

```
Declared for one optional effect that mixes the live camera into the film. Off by default; it starts only when the viewer presses W or uses the camera launch option. A red LIVE CAMERA label always shows while on, and W (outside Explore) turns it off. Pictures stay in memory; nothing is recorded, saved or sent, and the app's own F12 capture leaves the camera out. Windows also asks for permission first.
```

## Properties and policy fields

| Field | Value |
| --- | --- |
| Category | Entertainment (or Photo & video) |
| Privacy policy URL | `https://github.com/jbrick2070/boo-sah-sah/blob/main/PRIVACY.md` (required because of the camera capability) |
| Website | `https://github.com/jbrick2070/boo-sah-sah` |
| Support contact | `https://github.com/jbrick2070/boo-sah-sah/issues` (issues must be enabled on the repository) |
| Minimum hardware | Windows 11 22H2 or newer, x64, Direct3D 12 GPU (Feature Level 11_0 or higher with Shader Model 6.0), stereo audio output. Required graphics: Intel Iris Xe, AMD Radeon Vega 8 or NVIDIA GTX 1050 or better (older Intel HD/UHD not supported) |
| Recommended hardware | A ray-tracing GPU (NVIDIA RTX, AMD RDNA 2 or newer, Intel Arc) for the full image |
| Copyright and trademark | (c) 2026 Jeffrey A Brick. Code MIT; soundtrack and cue sheets CC BY-NC 4.0. Scanned textures and models are CC0 (Poly Haven); see LICENSES.md |

## Age rating questionnaire (IARC)

Answer **No** to all of these: violence, blood, fear or horror themes, sexual content, nudity, profanity, drugs, alcohol, tobacco, gambling, simulated gambling, user-generated content, online interaction, location sharing, in-app purchases, unrestricted web access. Expected result: lowest rating (3+ / Everyone). The film is spy-noir in tone and non-violent.

## Before you press Submit

- [ ] The privacy policy address opens in a private (signed-out) browser window with no sign-in. The website and support links do too.
- [ ] The package display name is exactly the reserved name (`Boo Sah Sah`) and the identity values were pasted from Product identity.
- [ ] The MSIX was built with `make-msix.ps1` and installed once with `-Register`: the film plays, audio works, F12 saves a picture, W shows the LIVE CAMERA label and turns off.
- [ ] Run once with `--force-warp` for about 30 seconds (software renderer, like a reviewer's virtual machine): it must not crash or hang. If it cannot start, the notes above stay accurate (a message, then exit).
- [ ] Screenshots are real captures of this build, 1920x1080 PNG, at least the minimum the Store asks for.
- [ ] Every language in the manifest has a listing in `store-listing.md` (the Store asks for one per declared language).
- [ ] Submit with **Publish manually** or a date a day ahead, so you can look at the certified package before it goes live.
