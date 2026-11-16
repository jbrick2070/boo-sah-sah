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

## Store notes

- The package install folder is read-only. The app writes nothing next to its exe by default (`log_file` is empty). The **capture** key writes `diagnostics/captures` relative to the working directory and can fail in the Store build; check it with `-Register` before submitting.
- `runFullTrust` is required (Win32 Direct3D 12 app that loads optional helper libraries from its own folder). `webcam` is declared for the opt-in camera input (key `W`), off by default.
- Windows 11 only (`MinVersion` 10.0.22000.0), as the minimum baseline in the spec.
- Age rating: no violence, no purchases, no online features. Not a game: choose an entertainment or photo and video category.
