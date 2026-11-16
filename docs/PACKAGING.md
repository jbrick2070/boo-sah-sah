# Packaging and distribution

One build, four ways to get BOO SAH SAH. All Windows x64, all from the same staged app folder.

| Format | Built by | Where it goes | Signing |
| --- | --- | --- | --- |
| Setup (`BooSahSah-Setup-win64.exe`) | `scripts/package_release.ps1` | GitHub release | Unsigned: SmartScreen warns |
| Portable zip (`boo-sah-sah-win64-portable.zip`) | `scripts/package_release.ps1` | GitHub release, Scoop | None |
| Microsoft Store (`BooSahSah_<ver>_x64.msix`) | `packaging/msix/make-msix.ps1` | Partner Center upload | The Store signs it |
| winget and Scoop manifests | `packaging/New-PackageManifests.ps1` | `microsoft/winget-pkgs`, a Scoop bucket | Point at the release files |

## Steps

1. `./scripts/package_release.ps1` builds, validates the cue and asset manifest, and writes the setup and zip to `build/dist`. It also stages `build/dist/boo-sah-sah-win64-app`.
2. Store package: reserve the name in Partner Center, copy the three **Product identity** values, then
   `./packaging/msix/make-msix.ps1 -IdentityName <Name> -Publisher "CN=<ID>" -PublisherDisplay "<Display>"`.
   Add `-Register` first to try it locally (Developer Mode). Upload the `.msix` in the submission.
3. Publish the GitHub release with the setup and zip. Then
   `./packaging/New-PackageManifests.ps1 -Version 1.0.0 -Tag v1.0.0` writes the winget and Scoop manifests with the real hashes.

## Languages

The Store package declares English plus 24 languages: French, German, Spanish, Italian, Portuguese (Brazil), Japanese, Korean, Chinese (Simplified and Traditional), Russian, Polish, Dutch, Turkish, Swedish, Arabic, Hindi, Thai, Vietnamese, Indonesian, Czech, Ukrainian, Danish, Norwegian and Finnish.

- `packaging/msix/descriptions.json` holds the package description per language. `make-msix.ps1` turns it into the manifest language list and `resources.pri`. To add a language, add one line there.
- `packaging/store-listing.md` holds the listing text to paste into Partner Center, one listing per language.
- `assets/strings/<tag>.json` holds everything the film itself shows: captions, the end card, HUD labels, the camera indicator and the window-title words. `en-US.json` is the source, with a note for the translator and a length limit on every string. The app follows the Windows display language; `--lang <tag>` (or `display.language` in the config) overrides it, and `--lang qps-ploc` shows an accented pseudo-language for layout checks.
- English is still drawn by the shader glyph tables, pixel for pixel as before. Other languages are drawn with Windows system fonts (DirectWrite: Arabic shaping and right-to-left, Devanagari, Thai, the right Han forms for Japanese, Korean and each Chinese) into a text atlas the HUD samples. No font is bundled.
- Left as authored in every language: the title `BOO SAH SAH`, the 台北 / TAIPEI motif, act numerals, soundtrack titles, numbers, and engineering tokens (FPS, GPU, RT, CUDA, NPU, NRD, SPP, AOV, MS).
- `src/app/localization.cpp` holds the two messages shown through Windows before or without the renderer (the unsupported-graphics box and the graphics-failure panel).
- To add a language: add `assets/strings/<tag>.json`, a row in `localization.cpp`, a line in `descriptions.json` and a section in `store-listing.md`. The unit test `UiStringCatalogs` then checks it is complete, keeps the protected tokens, and that every HUD label fits its box at every HUD size.
- The setup program (not part of the Store package) is English only.
- The translations are machine-made and checked by a second model. Have a native speaker read a language before promoting it.

## Store notes

- The package install folder is read-only. The app writes nothing next to its exe by default (`log_file` is empty). The **capture** key (F12) writes `diagnostics/captures` relative to the working directory; when that cannot be written (Start menu launch, Store package) it saves to `Pictures\BOO SAH SAH` instead. Checked with `-Register`.
- `runFullTrust` is required (Win32 Direct3D 12 app that loads optional helper libraries from its own folder). `webcam` is declared for the opt-in camera input (key `W`), off by default. Because of it the Store needs a privacy policy web address: use the public address of `PRIVACY.md`.
- Windows 11 only (`MinVersion` 10.0.22000.0), as the minimum baseline in the spec.
- Age rating: no violence, no purchases, no online features. Not a game: choose an entertainment or photo and video category.
