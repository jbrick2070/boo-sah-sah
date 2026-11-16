param(
    [ValidateSet("x64-base-release", "x64-base-debug")]
    [string]$Preset = "x64-base-release",
    [string]$OutRoot = "",
    [switch]$ExcludeOpenVinoRuntime,
    [switch]$ExcludeNrdRuntime
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = if ($Preset -eq "x64-base-debug") { Join-Path $RepoRoot "build\x64-debug" } else { Join-Path $RepoRoot "build\x64-release" }

if ([string]::IsNullOrWhiteSpace($OutRoot)) {
    $OutRoot = Join-Path $RepoRoot "build\dist"
}
$OutRoot = [IO.Path]::GetFullPath($OutRoot)

Write-Host "==> Building $Preset (single-app + native LZMS setup stub)" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "build.ps1") -Preset $Preset

$AppExe  = Join-Path $BuildDir "boo_sah_sah.exe"
$StubExe = Join-Path $BuildDir "boo_installer_stub.exe"

Write-Host "==> Validating asset & cue provenance manifest via unified boo_sah_sah.exe --validate-cues" -ForegroundColor Cyan
Push-Location $RepoRoot
try {
    & $AppExe --validate-cues --no-activate | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Cue/manifest validation failed before packaging!" }
} finally {
    Pop-Location
}

$StageDir  = Join-Path $OutRoot "boo-sah-sah-win64-app"
$SetupPath = Join-Path $OutRoot "BooSahSah-Setup-win64.exe"
$ZipPath   = Join-Path $OutRoot "boo-sah-sah-win64-portable.zip"

foreach ($old in @(
    $StageDir,
    (Join-Path $OutRoot "boo-sah-sah-win64-portable"),
    $SetupPath,
    $ZipPath
)) {
    $ResolvedOld = [IO.Path]::GetFullPath($old)
    if (-not $ResolvedOld.StartsWith($OutRoot.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing cleanup outside output root: $ResolvedOld"
    }
    if (Test-Path -LiteralPath $ResolvedOld) {
        Remove-Item -LiteralPath $ResolvedOld -Recurse -Force
    }
}

New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\shaders") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\meshes") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\textures") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\models") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\audio") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\cues") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "config") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "docs") | Out-Null

# 1. Single Unified Application Executable (strictly zero-SDK BASE_ONLY PE import table)
Copy-Item -Force $AppExe (Join-Path $StageDir "boo_sah_sah.exe")

# 2. Compiled DXIL shaders (.cso) + HLSL sources referenced by manifest.json
Copy-Item -Force (Join-Path $BuildDir "assets\shaders\*.cso") (Join-Path $StageDir "assets\shaders\")
Copy-Item -Force (Join-Path $RepoRoot "assets\shaders\*.hlsl") (Join-Path $StageDir "assets\shaders\")

# 3. Packed 3D meshes, PBR texture arrays, Madame N stage-director IR model, cue schedules, and provenance manifest
Copy-Item -Force (Join-Path $RepoRoot "assets\meshes\*.bmesh") (Join-Path $StageDir "assets\meshes\")
Copy-Item -Force (Join-Path $RepoRoot "assets\textures\*.btex") (Join-Path $StageDir "assets\textures\")
Copy-Item -Force (Join-Path $RepoRoot "assets\models\madame_n_*") (Join-Path $StageDir "assets\models\")
Copy-Item -Force (Join-Path $RepoRoot "assets\audio\*.mp3") (Join-Path $StageDir "assets\audio\")
Copy-Item -Force (Join-Path $RepoRoot "assets\cues\*.json") (Join-Path $StageDir "assets\cues\")
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\strings") | Out-Null
Copy-Item -Force (Join-Path $RepoRoot "assets\strings\*.json") (Join-Path $StageDir "assets\strings\")
Copy-Item -Force (Join-Path $RepoRoot "assets\manifest.json") (Join-Path $StageDir "assets\manifest.json")

# 4. Runtime configuration & quality presets
Copy-Item -Force (Join-Path $RepoRoot "config\default.toml") (Join-Path $StageDir "config\default.toml")
Copy-Item -Force (Join-Path $RepoRoot "config\quality_presets.json") (Join-Path $StageDir "config\quality_presets.json")

# 5. Optional Apache-2.0 OpenVINO C + Intel NPU plugin sidecar folder (`openvino\`)
#    Allows out-of-the-box Intel(R) AI Boost NPU execution on target PCs with zero Python/SDK setup,
#    while keeping boo_sah_sah.exe 100% zero-static-import so non-Intel PCs still launch cleanly.
$VenvOvDir = Join-Path $RepoRoot ".venv\Lib\site-packages\openvino\libs"
if ((-not $ExcludeOpenVinoRuntime) -and (Test-Path $VenvOvDir)) {
    $OvStage = Join-Path $StageDir "openvino"
    New-Item -ItemType Directory -Force -Path $OvStage | Out-Null
    $MinimalOvDlls = @(
        "openvino.dll",
        "openvino_c.dll",
        "openvino_ir_frontend.dll",
        "openvino_intel_npu_plugin.dll",
        "openvino_intel_npu_compiler_loader.dll",
        "openvino_intel_npu_vm_runtime.dll",
        "tbb12.dll",
        "tbbbind_2_5.dll",
        "tbbmalloc.dll"
    )
    foreach ($dll in $MinimalOvDlls) {
        $srcDll = Join-Path $VenvOvDir $dll
        if (Test-Path $srcDll) {
            Copy-Item -Force $srcDll (Join-Path $OvStage $dll)
        }
    }
    $OvLicense = Get-ChildItem (Join-Path $RepoRoot ".venv\Lib\site-packages\openvino-*.dist-info\*LICENSE*") -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($OvLicense) {
        Copy-Item -Force $OvLicense.FullName (Join-Path $OvStage "LICENSE-OpenVINO-Apache-2.0.txt")
    }
    Write-Host "==> Staged compact OpenVINO NPU runtime sidecar into openvino\" -ForegroundColor Cyan
}

# 5b. Optional NVIDIA Real-Time Denoisers (`nrd\NRD.dll` v4.18.0) dynamic DXIL compute sidecar
#     Loaded exclusively via LoadLibraryExW + GetProcAddress at runtime (zero static PE import on boo_sah_sah.exe),
#     enabling progressive NRD REFERENCE accumulation up to 4096 SPP in Interactive Explore Studio mode.
$NrdDllSrc = Join-Path $RepoRoot "third_party\nvidia_nrd\bin\NRD.dll"
$NrdLicSrc = Join-Path $RepoRoot "third_party\nvidia_nrd\LICENSE-NVIDIA-NRD.txt"
if ((-not $ExcludeNrdRuntime) -and (Test-Path $NrdDllSrc)) {
    $NrdStage = Join-Path $StageDir "nrd"
    New-Item -ItemType Directory -Force -Path $NrdStage | Out-Null
    Copy-Item -Force $NrdDllSrc (Join-Path $NrdStage "NRD.dll")
    if (Test-Path $NrdLicSrc) {
        Copy-Item -Force $NrdLicSrc (Join-Path $NrdStage "LICENSE-NVIDIA-NRD.txt")
    }
    Write-Host "==> Staged dynamic NVIDIA NRD v4.18.0 DXIL sidecar into nrd\" -ForegroundColor Cyan
}

# 6. Documentation, changelog, and licenses
Copy-Item -Force (Join-Path $RepoRoot "README.md") (Join-Path $StageDir "README.md")
Copy-Item -Force (Join-Path $RepoRoot "LICENSES.md") (Join-Path $StageDir "LICENSES.md")
Copy-Item -Force (Join-Path $RepoRoot "CHANGELOG.md") (Join-Path $StageDir "CHANGELOG.md")
Copy-Item -Force (Join-Path $RepoRoot "docs\USER_GUIDE.md") (Join-Path $StageDir "docs\USER_GUIDE.md")
Copy-Item -Force (Join-Path $RepoRoot "docs\SPEC_v4.md") (Join-Path $StageDir "docs\SPEC_v4.md")
Copy-Item -Force (Join-Path $RepoRoot "docs\quality_upgrade_report.md") (Join-Path $StageDir "docs\quality_upgrade_report.md")
Copy-Item -Force (Join-Path $RepoRoot "docs\quality_timings.json") (Join-Path $StageDir "docs\quality_timings.json")

Write-Host "==> Packing single-file Windows installer: $SetupPath" -ForegroundColor Cyan
$packProc = Start-Process -FilePath $StubExe -ArgumentList @("--pack", "`"$StubExe`"", "`"$StageDir`"", "`"$SetupPath`"") -Wait -PassThru -NoNewWindow
if ($packProc.ExitCode -ne 0 -or -not (Test-Path $SetupPath)) {
    throw "Failed packing single-file installer (exit code $($packProc.ExitCode))!"
}

Write-Host "==> Compressing companion portable ZIP archive: $ZipPath" -ForegroundColor Cyan
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -Force

# 7. Verify installs, upgrade preservation/replacement, rollback, and uninstall.
# Shell metacharacters are deliberate: native setup must treat paths literally.
$VerifyRoot = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) ("boo-setup-verify-" + [guid]::NewGuid().ToString("N"))))
$VerifyDir = Join-Path $VerifyRoot 'app space & 100% ! (test)'
$SavedPath = $env:PATH
$Succeeded = $false
function Invoke-Setup([string[]]$ExtraArguments = @(), [int]$ExpectedExit = 0) {
    $Arguments = @('--silent', '--install-dir', "`"$VerifyDir`"", '--no-shortcuts', '--no-registry', '--no-launch', '--no-activate') + $ExtraArguments
    $Process = Start-Process -FilePath $SetupPath -ArgumentList $Arguments -Wait -PassThru -WindowStyle Hidden
    if ($Process.ExitCode -ne $ExpectedExit) { throw "Setup expected exit $ExpectedExit, got $($Process.ExitCode)." }
}
try {
    $env:PATH = Join-Path $env:SystemRoot 'System32'
    Invoke-Setup
    $InstalledApp = Join-Path $VerifyDir "boo_sah_sah.exe"
    $Uninstaller  = Join-Path $VerifyDir "Uninstall-BooSahSah.exe"
    if (-not (Test-Path $InstalledApp) -or -not (Test-Path $Uninstaller)) {
        throw "Installed app or uninstaller missing after silent install!"
    }
    if ((-not $ExcludeNrdRuntime) -and (Test-Path $NrdDllSrc) -and -not (Test-Path (Join-Path $VerifyDir "nrd\NRD.dll"))) {
        throw "Installed nrd\NRD.dll sidecar missing after silent install!"
    }

    $Config = Join-Path $VerifyDir 'config\default.toml'
    [IO.File]::AppendAllText($Config, "`r`n# packaging upgrade preservation sentinel`r`n")
    $ConfigHash = (Get-FileHash -LiteralPath $Config).Hash
    $UserFile = Join-Path $VerifyDir 'user notes.txt'
    [IO.File]::WriteAllText($UserFile, 'Untracked user data must survive upgrade and uninstall.')
    Invoke-Setup @('--preserve-config')
    if ((Get-FileHash -LiteralPath $Config).Hash -ne $ConfigHash) { throw 'Upgrade overwrote config overrides.' }
    if (-not (Test-Path -LiteralPath $UserFile)) { throw 'Upgrade deleted untracked user data.' }

    # A locked late archive entry forces rollback after earlier files were replaced.
    $Readme = Join-Path $VerifyDir 'README.md'
    [IO.File]::AppendAllText($Readme, "`r`nRollback sentinel`r`n")
    $ReadmeHash = (Get-FileHash -LiteralPath $Readme).Hash
    $ManifestHash = (Get-FileHash -LiteralPath (Join-Path $VerifyDir 'install_manifest.txt')).Hash
    $LockedFile = [IO.File]::Open($Config, 'Open', 'Read', 'None')
    try { Invoke-Setup @('--replace-config') 1 } finally { $LockedFile.Dispose() }
    if ((Get-FileHash -LiteralPath $Readme).Hash -ne $ReadmeHash -or
        (Get-FileHash -LiteralPath $Config).Hash -ne $ConfigHash -or
        (Get-FileHash -LiteralPath (Join-Path $VerifyDir 'install_manifest.txt')).Hash -ne $ManifestHash) {
        throw 'Failed upgrade did not restore previous files and manifest.'
    }
    if (@(Get-ChildItem -LiteralPath $VerifyDir -Force -Filter '.boo-rollback-*').Count) { throw 'Rollback left transaction files.' }
    Invoke-Setup @('--replace-config')
    if ((Get-FileHash -LiteralPath $Config).Hash -ne (Get-FileHash -LiteralPath (Join-Path $StageDir 'config\default.toml')).Hash) {
        throw 'Explicit config replacement did not install packaged defaults.'
    }
    Push-Location $VerifyDir
    try {
        & $InstalledApp --validate-cues --no-activate | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Installed boo_sah_sah.exe --validate-cues failed!" }
    } finally {
        Pop-Location
    }

    $TrackedFiles = @(Get-Content -LiteralPath (Join-Path $VerifyDir 'install_manifest.txt') |
        Where-Object { $_.StartsWith('FILE=') } | ForEach-Object { $_.Substring(5) })
    # A locked tracked file must fail synchronously and leave a retryable uninstall.
    $ManifestPath = Join-Path $VerifyDir 'install_manifest.txt'
    $ManifestHash = (Get-FileHash -LiteralPath $ManifestPath).Hash
    $LockedFile = [IO.File]::Open($Config, 'Open', 'Read', 'None')
    try {
        $uninstProc = Start-Process -FilePath $Uninstaller -ArgumentList @('--uninstall', '--silent', '--no-registry') -Wait -PassThru -WindowStyle Hidden
        if ($uninstProc.ExitCode -ne 6) { throw "Locked-file uninstall expected exit 6, got $($uninstProc.ExitCode)." }
        if (-not (Test-Path -LiteralPath $Uninstaller) -or
            -not (Test-Path -LiteralPath $ManifestPath) -or
            (Get-FileHash -LiteralPath $ManifestPath).Hash -ne $ManifestHash) {
            throw 'Failed uninstall did not retain its executable and manifest for retry.'
        }
        if (-not (Test-Path -LiteralPath $UserFile)) { throw 'Failed uninstall deleted untracked user data.' }
    } finally { $LockedFile.Dispose() }
    $uninstProc = Start-Process -FilePath $Uninstaller -ArgumentList @("--uninstall", "--silent", "--no-registry") -Wait -PassThru -WindowStyle Hidden
    if ($uninstProc.ExitCode -ne 0) { throw "Silent uninstaller failed ($($uninstProc.ExitCode))!" }
    $RemainingContent = @($TrackedFiles | Where-Object { $_ -ine 'Uninstall-BooSahSah.exe' -and (Test-Path -LiteralPath (Join-Path $VerifyDir $_)) })
    if ($RemainingContent.Count -or (Test-Path -LiteralPath $ManifestPath)) {
        throw "Uninstall reported success before removing tracked content: $($RemainingContent -join ', ')"
    }
    # Only the original uninstaller image may remain until the native helper runs.
    $Deadline = [DateTime]::UtcNow.AddSeconds(30)
    do {
        $Remaining = @($TrackedFiles | Where-Object { Test-Path -LiteralPath (Join-Path $VerifyDir $_) })
        if ($Remaining.Count -eq 0 -and -not (Test-Path -LiteralPath (Join-Path $VerifyDir 'install_manifest.txt'))) { break }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $Deadline)
    if ($Remaining.Count -or (Test-Path -LiteralPath (Join-Path $VerifyDir 'install_manifest.txt'))) {
        throw "Uninstaller left tracked files: $($Remaining -join ', ')"
    }
    if (-not (Test-Path -LiteralPath $UserFile)) { throw 'Uninstaller deleted untracked user data.' }
    $Succeeded = $true
} finally {
    $env:PATH = $SavedPath
    # Retain failed verification artifacts; never mask an uninstall failure.
    if ($Succeeded -and (Test-Path -LiteralPath $VerifyRoot)) {
        $ResolvedVerifyRoot = [IO.Path]::GetFullPath($VerifyRoot)
        $TempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
        if (-not $ResolvedVerifyRoot.StartsWith($TempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            -not ([IO.Path]::GetFileName($ResolvedVerifyRoot)).StartsWith('boo-setup-verify-')) { throw 'Unsafe verification cleanup path.' }
        Remove-Item -LiteralPath $ResolvedVerifyRoot -Recurse -Force
    } elseif (-not $Succeeded) { Write-Warning "Verification artifacts retained at $VerifyRoot" }
}

$SetupSizeMb = [math]::Round((Get-Item $SetupPath).Length / 1MB, 2)
$ZipSizeMb   = [math]::Round((Get-Item $ZipPath).Length / 1MB, 2)
Write-Host "[PASS] Built single-app installer: $SetupPath ($SetupSizeMb MB) | Portable ZIP: $ZipPath ($ZipSizeMb MB)" -ForegroundColor Green
