# Microsoft Store: certification and submission answers

Paste each block into the matching Partner Center field. Limits are Partner Center's: notes for certification 2,000 characters, restricted capability justification 500.

> The repository is public. The privacy policy address below works once `PRIVACY.md` is on `main` (PR "License and public privacy policy"). Check it in a signed-out browser window before submitting.

## Notes for certification (Submission options)

```
BOO SAH SAH is an interactive six-minute film rendered on the viewer's PC. It works offline in 25 languages. The first launch can take longer while the graphics driver prepares shaders. The loading screen shows progress.

1. Press F to explore. Fly with W A S D, look by dragging the right mouse button, and press Y to drop marbles. Press F to return to the film.
2. Open Hardware Orchestra and choose Run benchmark, or press Ctrl+B. Select Raster and run it. The film pauses for the fixed shot. Read the GPU frame times and the rendering paths that ran. Save picture and text lets you choose a local folder. Back to film restores playback. The report form opens only after confirmation.
3. In the same selector, path tracing and DLSS Quality have separate tests. Unavailable choices explain their requirements. Ray tracing is optional for the film and raster test.
4. Space pauses the film. Left and Right seek. N and P change acts. Z changes the music. Tab changes the panel and F11 toggles full screen.
5. F12 saves a film frame. The optional camera starts only on request and shows a red LIVE CAMERA label. Press W outside Explore to stop it.

Requires Direct3D 12 and Shader Model 6.0. Integrated and AMD graphics use a lighter startup profile.
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
| Minimum hardware | Windows 11 22H2 or newer, x64, Direct3D 12 GPU (Feature Level 11_0 or higher with Shader Model 6.0), stereo audio output |
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
