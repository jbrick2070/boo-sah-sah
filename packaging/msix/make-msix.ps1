# Builds the Microsoft Store package (MSIX) of BOO SAH SAH from the staged app folder that scripts\package_release.ps1
# produces (build\dist\boo-sah-sah-win64-app). The package is unsigned: the Store signs it after certification.
# Use -Register for a local try instead (needs Developer Mode; remove with Remove-AppxPackage).
#
#   .\scripts\package_release.ps1
#   .\packaging\msix\make-msix.ps1 -IdentityName "<Package/Identity/Name>" -Publisher "CN=<Publisher ID>" -PublisherDisplay "<Publisher display name>"
#
# The three Partner Center values are in Product management > Product identity; they must match exactly.
param(
  [Parameter(Mandatory)][string]$IdentityName,
  [Parameter(Mandatory)][string]$Publisher,
  [Parameter(Mandatory)][string]$PublisherDisplay,
  [string]$Version = '1.0.0.0',
  [string]$Stage,
  [string]$OutDir,
  [switch]$Register,
  [switch]$Overwrite
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Stage) { $Stage = Join-Path $repo 'build\dist\boo-sah-sah-win64-app' }
if (-not $OutDir) { $OutDir = Join-Path $repo 'build\dist' }
if ($Version -notmatch '^\d{1,5}\.\d{1,5}\.\d{1,5}\.0$') { throw "Version must look like 1.0.0.0 (the last number 0), not $Version." }
if ($Publisher -notmatch '^CN=') { throw 'Publisher must start with CN= (copy it from Partner Center Product identity).' }
if ($IdentityName -notmatch '^[A-Za-z0-9.-]{3,50}$') { throw "IdentityName looks wrong: $IdentityName" }
foreach ($need in 'boo_sah_sah.exe', 'assets\manifest.json', 'config\default.toml') {
  if (-not (Test-Path -LiteralPath (Join-Path $Stage $need))) { throw "$Stage has no $need. Run scripts\package_release.ps1 first." }
}
$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'x64\makeappx.exe') } |
  Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (-not $sdk) { throw 'The Windows SDK (makeappx.exe, makepri.exe) is needed.' }
$makeappx = Join-Path $sdk.FullName 'x64\makeappx.exe'; $makepri = Join-Path $sdk.FullName 'x64\makepri.exe'

$out = [IO.Path]::GetFullPath($OutDir); New-Item -ItemType Directory -Force -Path $out | Out-Null
$msix = Join-Path $out "BooSahSah_${Version}_x64.msix"
if ((Test-Path -LiteralPath $msix) -and -not $Overwrite) { throw "$msix exists. Raise -Version for a new upload, or pass -Overwrite." }
$layout = Join-Path $out "msix-layout-$Version"
if (Test-Path -LiteralPath $layout) {
  if ((Get-Item -LiteralPath $layout -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "$layout is a link. Remove it by hand." }
  Remove-Item -LiteralPath $layout -Recurse -Force
}
New-Item -ItemType Directory -Path $layout | Out-Null
Copy-Item -Path (Join-Path $Stage '*') -Destination $layout -Recurse
New-Item -ItemType Directory -Path (Join-Path $layout 'Assets') | Out-Null
Copy-Item -Path (Join-Path $PSScriptRoot 'assets\*.png') -Destination (Join-Path $layout 'Assets')
$manifest = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'AppxManifest.xml')).
  Replace('$IDENTITY_NAME$', $IdentityName).Replace('$PUBLISHER$', $Publisher).Replace('$PUBLISHER_DISPLAY$', $PublisherDisplay).Replace('$VERSION$', $Version)
[IO.File]::WriteAllText((Join-Path $layout 'AppxManifest.xml'), $manifest, (New-Object Text.UTF8Encoding($false)))

$priconfig = Join-Path $out "priconfig-$Version.xml"
& $makepri createconfig /cf $priconfig /dq en-US /pv 10.0.0 /o | Out-Null
if ($LASTEXITCODE) { throw 'makepri createconfig failed.' }
[xml]$pri = [IO.File]::ReadAllText($priconfig)
foreach ($node in @($pri.SelectNodes('//packaging'))) { [void]$node.ParentNode.RemoveChild($node) }
$pri.Save($priconfig)
& $makepri new /pr $layout /cf $priconfig /mn (Join-Path $layout 'AppxManifest.xml') /of (Join-Path $layout 'resources.pri') /o | Out-Null
if ($LASTEXITCODE) { throw 'makepri new failed.' }
Remove-Item -LiteralPath $priconfig -Force
$extra = Get-ChildItem -LiteralPath $layout -Filter '*.pri' | Where-Object Name -ne 'resources.pri'
if ($extra) { throw "makepri split the resources ($($extra.Name -join ', ')); the package needs one resources.pri." }

& $makeappx pack /d $layout /p $msix /o /h SHA256 | Out-Null
if ($LASTEXITCODE) { throw 'makeappx pack failed (run it by hand to see why).' }
Write-Host "Package: $msix ($([math]::Round((Get-Item $msix).Length / 1MB, 1)) MB)"
if ($Register) {
  Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml')
  $pkg = Get-AppxPackage -Name $IdentityName
  Write-Host "Registered $($pkg.PackageFullName). Start it: explorer.exe shell:AppsFolder\$($pkg.PackageFamilyName)!App"
}
