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

function Assert-SameSet([string]$label, [string[]]$expected, [string[]]$found) {
  $missing = @($expected | Where-Object { $found -cnotcontains $_ })
  $extra = @($found | Where-Object { $expected -cnotcontains $_ })
  if ($missing.Count -or $extra.Count -or $expected.Count -ne $found.Count) {
    throw "$label differs (expected $($expected.Count), found $($found.Count)). Missing: $($missing -join ', '). Extra: $($extra -join ', ')."
  }
}
function Read-PackedText($entry) {
  $reader = [IO.StreamReader]::new($entry.Open())
  try { $reader.ReadToEnd() } finally { $reader.Dispose() }
}
function Assert-StorePackage([string]$path) {
  Add-Type -AssemblyName System.IO.Compression.FileSystem
  $archive = [IO.Compression.ZipFile]::OpenRead($path)
  try {
    $entries = @{}
    foreach ($entry in $archive.Entries) { $entries[$entry.FullName] = $entry }
    $required = @(
      'AppxManifest.xml', 'resources.pri', 'boo_sah_sah.exe', 'assets/manifest.json', 'config/default.toml',
      'LICENSE', 'LICENSE-ASSETS.md', 'LICENSES.md',
      'config/quality_presets.json', 'assets/cues/film_360s.json', 'assets/models/madame_n_director.xml',
      'assets/models/madame_n_director.bin', 'assets/meshes/shenzhen_evtol.bmesh',
      'assets/shaders/m1_present_hud.cso', 'assets/shaders/m1_composite.cso', 'assets/shaders/m1_particles_vs.cso',
      'assets/shaders/m1_particles_ps.cso', 'assets/shaders/m2_mesh_vs.cso', 'assets/shaders/m2_mesh_ps.cso',
      'assets/shaders/m2_mesh_rt_ps.cso', 'assets/shaders/m1_nv12_pack_cs.cso'
    )
    foreach ($name in $required) {
      if ($entries[$name].Length -le 0) { throw "Missing or empty Store payload: $name" }
    }
    [xml]$appManifest = Read-PackedText $entries['AppxManifest.xml']
    $identity = $appManifest.Package.Identity
    if ($identity.Name -cne $IdentityName -or $identity.Publisher -cne $Publisher -or
        $identity.Version -ne $Version -or $identity.ProcessorArchitecture -ne 'x64') {
      throw 'The packed manifest differs from the requested x64 Store identity or version.'
    }
    $sourceTags = @(Get-ChildItem -LiteralPath (Join-Path $repo 'assets/strings') -File -Filter '*.json' |
                    ForEach-Object { $_.BaseName })
    Assert-SameSet 'Store descriptions and supported runtime languages' $sourceTags $languages
    $manifestTags = @($appManifest.Package.Resources.Resource | ForEach-Object { $_.Language.ToLowerInvariant() })
    Assert-SameSet 'MSIX manifest languages' @($languages | ForEach-Object { $_.ToLowerInvariant() }) $manifestTags
    $tags = @($entries.Keys | Where-Object { $_ -match '^assets/strings/[^/]+\.json$' } |
              ForEach-Object { [IO.Path]::GetFileNameWithoutExtension($_) })
    Assert-SameSet 'Runtime catalog languages' $languages $tags
    $listings = [IO.File]::ReadAllText((Join-Path $repo 'packaging/store-listing.md'))
    $listingTags = @([regex]::Matches($listings, '(?m)^## ([a-z]{2}-[A-Z]{2})[ \t]*\r?$') |
                     ForEach-Object { $_.Groups[1].Value })
    Assert-SameSet 'Store listing languages' $languages $listingTags

    $english = [IO.File]::ReadAllText((Join-Path $repo 'assets/strings/en-US.json')) | ConvertFrom-Json
    $keys = @($english.strings.PSObject.Properties.Name)
    foreach ($tag in $tags) {
      $catalog = (Read-PackedText $entries["assets/strings/$tag.json"]) | ConvertFrom-Json
      if ($catalog.language -cne $tag) { throw "Catalog $tag declares language '$($catalog.language)'." }
      Assert-SameSet "$tag string keys" $keys @($catalog.strings.PSObject.Properties.Name)
      foreach ($item in $catalog.strings.PSObject.Properties) {
        $value = if ($tag -ceq 'en-US') { $item.Value.text } else { $item.Value }
        if ($value -isnot [string] -or [string]::IsNullOrWhiteSpace($value)) { throw "$tag has no translation for $($item.Name)." }
      }
    }
    $exeStream = $entries['boo_sah_sah.exe'].Open()
    try { $exeHash = (Get-FileHash -InputStream $exeStream -Algorithm SHA256).Hash } finally { $exeStream.Dispose() }
    if ($exeHash -ne (Get-FileHash -LiteralPath (Join-Path $Stage 'boo_sah_sah.exe')).Hash) {
      throw 'The Store executable differs from the staged Release executable.'
    }
    $stageShaders = @(Get-ChildItem -LiteralPath (Join-Path $Stage 'assets/shaders') -File -Filter '*.cso')
    $packedShaders = @($entries.Keys | Where-Object { $_ -match '^assets/shaders/[^/]+\.cso$' })
    Assert-SameSet 'Compiled renderer shaders' @($stageShaders | ForEach-Object { 'assets/shaders/' + $_.Name }) $packedShaders
    foreach ($shader in $stageShaders) {
      $entry = $entries['assets/shaders/' + $shader.Name]
      if ($entry.Length -le 0) { throw "Empty compiled renderer shader: $($shader.Name)" }
      $stream = $entry.Open()
      try { $hash = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash } finally { $stream.Dispose() }
      if ($hash -ne (Get-FileHash -LiteralPath $shader.FullName).Hash) {
        throw "Store shader differs from the staged build: $($shader.Name)"
      }
    }
    $assets = (Read-PackedText $entries['assets/manifest.json']) | ConvertFrom-Json
    $soundtrack = @($assets.assets | Where-Object { $_.kind -eq 'audio' } | ForEach-Object { $_.path })
    Assert-SameSet 'Packaged soundtrack files' $soundtrack @($entries.Keys | Where-Object { $_ -match '^assets/audio/[^/]+$' })
    foreach ($asset in $assets.assets) {
      $entry = $entries[$asset.path]
      if ($entry.Length -le 0) { throw "Missing or empty registered asset: $($asset.path)" }
      $stream = $entry.Open()
      try { $hash = (Get-FileHash -InputStream $stream -Algorithm SHA256).Hash } finally { $stream.Dispose() }
      if ($hash -ne $asset.sha256) { throw "Store asset SHA-256 mismatch: $($asset.path)" }
    }
  } finally { $archive.Dispose() }
}

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
# The staged app already has an assets folder (shaders, meshes); the logos go beside them.
New-Item -ItemType Directory -Force -Path (Join-Path $layout 'Assets') | Out-Null
Copy-Item -Path (Join-Path $PSScriptRoot 'assets\*.png') -Destination (Join-Path $layout 'Assets')
# Languages: descriptions.json maps a language tag to the package description. en-US is first and is the default.
$descriptions = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'descriptions.json')) | ConvertFrom-Json
$languages = @($descriptions.PSObject.Properties.Name)
if ($languages[0] -ne 'en-US') { throw 'descriptions.json must list en-US first (the default language).' }
foreach ($lang in $languages) {
  if ($lang -notmatch '^[a-z]{2}-[A-Z]{2}$') { throw "descriptions.json: bad language tag $lang" }
  $text = [string]$descriptions.$lang
  if (-not $text.Trim() -or $text.Length -gt 2048) { throw "descriptions.json: the $lang description is empty or over 2048 characters." }
  $dir = Join-Path $layout "Strings\$lang"; New-Item -ItemType Directory -Force -Path $dir | Out-Null
  $resw = "<?xml version=`"1.0`" encoding=`"utf-8`"?>`n<root>`n  <data name=`"AppDescription`" xml:space=`"preserve`"><value>$([Security.SecurityElement]::Escape($text))</value></data>`n</root>`n"
  [IO.File]::WriteAllText((Join-Path $dir 'Resources.resw'), $resw, (New-Object Text.UTF8Encoding($false)))
}
$resources = ($languages | ForEach-Object { "    <Resource Language=`"$($_.ToLowerInvariant())`" />" }) -join "`n"
$manifest = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'AppxManifest.xml')).
  Replace('$IDENTITY_NAME$', $IdentityName).Replace('$PUBLISHER$', $Publisher).Replace('$PUBLISHER_DISPLAY$', $PublisherDisplay).Replace('$VERSION$', $Version).
  Replace('$RESOURCES$', $resources)
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
Remove-Item -LiteralPath (Join-Path $layout 'Strings') -Recurse -Force   # compiled into resources.pri; not needed in the package

$pending = Join-Path $out ("candidate-{0}.msix" -f [Guid]::NewGuid())
try {
  & $makeappx pack /d $layout /p $pending /o /h SHA256 | Out-Null
  if ($LASTEXITCODE) { throw 'makeappx pack failed (run it by hand to see why).' }
  Assert-StorePackage $pending
  Move-Item -LiteralPath $pending -Destination $msix -Force:$Overwrite
} finally {
  if (Test-Path -LiteralPath $pending) { Remove-Item -LiteralPath $pending -Force }
}
Write-Host "Package: $msix ($([math]::Round((Get-Item $msix).Length / 1MB, 1)) MB)"
if ($Register) {
  Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml')
  $pkg = Get-AppxPackage -Name $IdentityName
  Write-Host "Registered $($pkg.PackageFullName). Start it: explorer.exe shell:AppsFolder\$($pkg.PackageFamilyName)!App"
}
