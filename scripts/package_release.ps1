param(
    [string]$Preset = "x64-base-release",
    [string]$OutRoot = "",
    [switch]$ExcludeOpenVinoRuntime
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = if ($Preset -eq "x64-base-debug") { Join-Path $RepoRoot "build\x64-debug" } else { Join-Path $RepoRoot "build\x64-release" }

if ([string]::IsNullOrWhiteSpace($OutRoot)) {
    $OutRoot = Join-Path $RepoRoot "build\dist"
}

Write-Host "==> Building $Preset (single-app + native LZMS setup stub)" -ForegroundColor Cyan
& (Join-Path $PSScriptRoot "build.ps1") -Preset $Preset

$AppExe  = Join-Path $BuildDir "boo_sah_sah.exe"
$StubExe = Join-Path $BuildDir "boo_installer_stub.exe"

Write-Host "==> Validating asset & cue provenance manifest via unified boo_sah_sah.exe --validate-cues" -ForegroundColor Cyan
Push-Location $RepoRoot
try {
    & $AppExe --validate-cues
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
    if (Test-Path $old) {
        Remove-Item -Recurse -Force $old
    }
}

New-Item -ItemType Directory -Force -Path $StageDir | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\shaders") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\meshes") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\models") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "assets\cues") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "config") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $StageDir "docs") | Out-Null

# 1. Single Unified Application Executable (strictly zero-SDK BASE_ONLY PE import table)
Copy-Item -Force $AppExe (Join-Path $StageDir "boo_sah_sah.exe")

# 2. Compiled DXIL shaders (.cso) + HLSL sources referenced by manifest.json
Copy-Item -Force (Join-Path $BuildDir "assets\shaders\*.cso") (Join-Path $StageDir "assets\shaders\")
Copy-Item -Force (Join-Path $RepoRoot "assets\shaders\*.hlsl") (Join-Path $StageDir "assets\shaders\")

# 3. Packed 3D meshes, Madame N stage-director IR model, cue schedules, and provenance manifest
Copy-Item -Force (Join-Path $RepoRoot "assets\meshes\*.bmesh") (Join-Path $StageDir "assets\meshes\")
Copy-Item -Force (Join-Path $RepoRoot "assets\models\madame_n_*") (Join-Path $StageDir "assets\models\")
Copy-Item -Force (Join-Path $RepoRoot "assets\cues\*.json") (Join-Path $StageDir "assets\cues\")
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

# 6. Documentation, changelog, and licenses
Copy-Item -Force (Join-Path $RepoRoot "README.md") (Join-Path $StageDir "README.md")
Copy-Item -Force (Join-Path $RepoRoot "LICENSES.md") (Join-Path $StageDir "LICENSES.md")
Copy-Item -Force (Join-Path $RepoRoot "CHANGELOG.md") (Join-Path $StageDir "CHANGELOG.md")
Copy-Item -Force (Join-Path $RepoRoot "docs\USER_GUIDE.md") (Join-Path $StageDir "docs\USER_GUIDE.md")
Copy-Item -Force (Join-Path $RepoRoot "docs\SPEC_v4.md") (Join-Path $StageDir "docs\SPEC_v4.md")

Write-Host "==> Packing single-file Windows installer: $SetupPath" -ForegroundColor Cyan
$packProc = Start-Process -FilePath $StubExe -ArgumentList @("--pack", "`"$StubExe`"", "`"$StageDir`"", "`"$SetupPath`"") -Wait -PassThru -NoNewWindow
if ($packProc.ExitCode -ne 0 -or -not (Test-Path $SetupPath)) {
    throw "Failed packing single-file installer (exit code $($packProc.ExitCode))!"
}

Write-Host "==> Compressing companion portable ZIP archive: $ZipPath" -ForegroundColor Cyan
Compress-Archive -Path (Join-Path $StageDir "*") -DestinationPath $ZipPath -Force

# 7. End-to-end clean-environment verification of the single-file installer + uninstaller
$VerifyDir = Join-Path $env:TEMP ("boo-setup-verify-" + [guid]::NewGuid().ToString("N").Substring(0, 8))
$SavedPath = $env:PATH
try {
    $env:PATH = "C:\Windows\System32;C:\Windows"
    $instProc = Start-Process -FilePath $SetupPath -ArgumentList @("--silent", "--install-dir", "`"$VerifyDir`"", "--no-shortcuts", "--no-registry", "--no-launch") -Wait -PassThru -NoNewWindow
    if ($instProc.ExitCode -ne 0) { throw "Silent setup install verification failed ($($instProc.ExitCode))!" }

    $InstalledApp = Join-Path $VerifyDir "boo_sah_sah.exe"
    $Uninstaller  = Join-Path $VerifyDir "Uninstall-BooSahSah.exe"
    if (-not (Test-Path $InstalledApp) -or -not (Test-Path $Uninstaller)) {
        throw "Installed app or uninstaller missing after silent install!"
    }

    Push-Location $VerifyDir
    try {
        & $InstalledApp --validate-cues | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Installed boo_sah_sah.exe --validate-cues failed!" }
    } finally {
        Pop-Location
    }

    $uninstProc = Start-Process -FilePath $Uninstaller -ArgumentList @("--uninstall", "--silent", "--no-registry") -Wait -PassThru -NoNewWindow
    if ($uninstProc.ExitCode -ne 0) { throw "Silent uninstaller failed ($($uninstProc.ExitCode))!" }
} finally {
    $env:PATH = $SavedPath
    Start-Sleep -Milliseconds 400
    if (Test-Path $VerifyDir) {
        Remove-Item -Recurse -Force $VerifyDir -ErrorAction SilentlyContinue
    }
}

$SetupSizeMb = [math]::Round((Get-Item $SetupPath).Length / 1MB, 2)
$ZipSizeMb   = [math]::Round((Get-Item $ZipPath).Length / 1MB, 2)
Write-Host "[PASS] Built single-app installer: $SetupPath ($SetupSizeMb MB) | Portable ZIP: $ZipPath ($ZipSizeMb MB)" -ForegroundColor Green
