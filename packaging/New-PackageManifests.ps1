# Writes the winget manifests and the Scoop manifest for a PUBLISHED GitHub release of BOO SAH SAH.
# It downloads the setup and the portable zip from the release, hashes them, and fills in the templates.
#
#   .\packaging\New-PackageManifests.ps1 -Version 1.0.0 -Tag v1.0.0 -OutDir build\manifests
#
# Expects the release to hold BooSahSah-Setup-win64.exe and boo-sah-sah-win64-portable.zip (scripts\package_release.ps1).
# Then submit build\manifests\winget\* to microsoft/winget-pkgs (wingetcreate or a PR) and commit boo-sah-sah.json to a Scoop bucket.
param(
  [Parameter(Mandatory)][string]$Version,
  [Parameter(Mandatory)][string]$Tag,
  [string]$Repo = 'jbrick2070/boo-sah-sah',
  [string]$OutDir = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build\manifests')
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "Version must look like 1.0.0, not $Version." }
$base = "https://github.com/$Repo/releases/download/$Tag"
$tmp = Join-Path ([IO.Path]::GetTempPath()) "boo-pkg-$([guid]::NewGuid().ToString('N'))"
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
  $hash = @{}
  foreach ($f in 'BooSahSah-Setup-win64.exe', 'boo-sah-sah-win64-portable.zip') {
    $p = Join-Path $tmp $f
    Invoke-WebRequest -Uri "$base/$f" -OutFile $p -UseBasicParsing
    $hash[$f] = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToUpperInvariant()
    Write-Host "$f $($hash[$f])"
  }
} finally { Remove-Item -LiteralPath $tmp -Recurse -Force -ErrorAction SilentlyContinue }

$id = 'jbrick2070.BooSahSah'
$w = Join-Path $OutDir 'winget'; New-Item -ItemType Directory -Force -Path $w | Out-Null
$date = (Get-Date).ToString('yyyy-MM-dd')
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $w "$id.yaml"), @"
PackageIdentifier: $id
PackageVersion: $Version
DefaultLocale: en-US
ManifestType: version
ManifestVersion: 1.6.0
"@, $utf8)
[IO.File]::WriteAllText((Join-Path $w "$id.locale.en-US.yaml"), @"
PackageIdentifier: $id
PackageVersion: $Version
PackageLocale: en-US
Publisher: BOO SAH SAH Project
PublisherUrl: https://github.com/$Repo
PackageName: BOO SAH SAH
PackageUrl: https://github.com/$Repo
License: MIT
LicenseUrl: https://github.com/$Repo/blob/main/LICENSE
ShortDescription: A 360-second real-time demo for Windows, rendered live with Direct3D 12 ray tracing.
Tags:
- demo
- direct3d12
- ray-tracing
- demoscene
- real-time
ManifestType: defaultLocale
ManifestVersion: 1.6.0
"@, $utf8)
[IO.File]::WriteAllText((Join-Path $w "$id.installer.yaml"), @"
PackageIdentifier: $id
PackageVersion: $Version
InstallerType: exe
Scope: user
InstallerSwitches:
  Silent: --silent --no-launch
  SilentWithProgress: --silent --no-launch
UpgradeBehavior: install
ReleaseDate: $date
Installers:
- Architecture: x64
  InstallerUrl: $base/BooSahSah-Setup-win64.exe
  InstallerSha256: $($hash['BooSahSah-Setup-win64.exe'])
ManifestType: installer
ManifestVersion: 1.6.0
"@, $utf8)

$s = @{
  version = $Version
  description = 'A 360-second real-time demo for Windows, rendered live with Direct3D 12 ray tracing.'
  homepage = "https://github.com/$Repo"
  license = 'MIT'
  architecture = @{ '64bit' = @{ url = "$base/boo-sah-sah-win64-portable.zip"; hash = $hash['boo-sah-sah-win64-portable.zip'].ToLowerInvariant() } }
  shortcuts = @(, @('boo_sah_sah.exe', 'BOO SAH SAH'))
  checkver = 'github'
  autoupdate = @{ architecture = @{ '64bit' = @{ url = "https://github.com/$Repo/releases/download/v`$version/boo-sah-sah-win64-portable.zip" } } }
}
[IO.File]::WriteAllText((Join-Path $OutDir 'boo-sah-sah.json'), ($s | ConvertTo-Json -Depth 6), $utf8)
Write-Host "Wrote winget manifests to $w and Scoop manifest to $(Join-Path $OutDir 'boo-sah-sah.json')"
