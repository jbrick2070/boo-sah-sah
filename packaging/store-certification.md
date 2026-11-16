# Microsoft Store: certification and submission answers

Paste each block into the matching Partner Center field. Limits are Partner Center's: notes for certification 2,000 characters, restricted capability justification 500.

> **Blocker: the GitHub repository is private.** The Store must be able to open the privacy policy address without signing in, and the website and support links must work for customers. A link into this repository fails certification. Before submitting, either make the repository public, or publish `PRIVACY.md` at any public address (for example a public GitHub page or your own site) and use that address below.

## Notes for certification (Submission options)

```
BOO SAH SAH is a six-minute (360 s) real-time film for Windows 11. It is not a game: there is no gameplay, no score, no sign-in, no ads, no purchases and no network use. The app works fully offline.

To test:
1. Turn the sound on and launch the app from the Start menu. The film starts by itself and plays in a window. Press F11 to toggle fullscreen and Esc at any time to exit.
2. Space pauses and resumes. Left and Right arrows seek 2 seconds. N and P jump between acts. E switches between the Family and Midnight editions. Tab cycles the on-screen display (Clean Cinema hides it).
3. The app picks the language of Windows; its captions and on-screen text exist in 25 languages. Everything works without any setting.

Hardware: it needs a Direct3D 12 graphics device (any DirectX 12 GPU, discrete or integrated). Ray tracing, NVIDIA CUDA, the Intel NPU and hardware video encoding are optional and detected at launch; without them the app uses built-in substitutes, so it runs on any DirectX 12 PC. On a PC with no usable Direct3D 12 adapter the app shows a plain message that says so and closes; this is expected. A virtual machine may run it slowly.

Camera: declared only for an optional effect. The camera is OFF by default and nothing uses it unless the viewer presses W. While it is on, a red LIVE CAMERA label is always visible; W turns it off at once. Camera pictures stay in memory and are never saved or sent. Privacy policy: see the link in the listing.

F12 saves a still picture of the current frame to Pictures\BOO SAH SAH. The app writes nothing else unless the viewer asks for it.

No account, login or special hardware is needed to test.
```

## Restricted capability: runFullTrust (justification, 500 characters at most)

```
BOO SAH SAH is a Win32 Direct3D 12 desktop app (C++). Full trust lets it use the GPU directly through D3D12 and DXGI, play audio through WASAPI, and load optional GPU helper libraries (NVIDIA denoiser, OpenVINO) from its own install folder at launch. It makes no internet connection, collects no data and does not change system settings.
```

## Device capability: webcam

```
Declared for one optional effect that mixes the live camera into the film. It is off by default and starts only when the viewer presses W. A red LIVE CAMERA indicator is always shown while it is on, and W turns it off immediately. Pictures stay in memory; nothing is recorded, saved or sent. Windows also asks for permission first.
```

## Properties and policy fields

| Field | Value |
| --- | --- |
| Category | Entertainment (or Photo & video) |
| Privacy policy URL | A public address of `PRIVACY.md` (required because of the camera capability). Not a link into the private repository |
| Website | Optional. A public page, or leave empty |
| Support contact | A public page or an email address you choose to publish |
| Minimum hardware | Windows 11 22H2 or newer, x64, Direct3D 12 GPU (Feature Level 11_0 or higher), stereo audio output |
| Recommended hardware | A ray-tracing GPU (NVIDIA RTX, AMD RDNA 2 or newer, Intel Arc) for the full image |
| Copyright and trademark | (c) 2026 Jeffrey A Brick. Original film, procedural art and music. Scanned textures and models are CC0 (Poly Haven); see LICENSES.md |

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
