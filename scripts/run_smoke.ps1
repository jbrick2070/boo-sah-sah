param(
    [string]$Preset = "x64-base-release",
    [double]$SmokeDurationSeconds = 3.5,
    [switch]$SkipBenchmarks,
    [switch]$LowPowerGpu,
    [string]$CompareRef,
    [double]$CompareMaxMean,
    [string[]]$CompareMap,
    [string]$CompareBaseline
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

# Validate comparison inputs before starting the build or any captures.
$hasCompareRef = $PSBoundParameters.ContainsKey('CompareRef')
if (-not $hasCompareRef) {
    foreach ($name in @('CompareMaxMean', 'CompareMap', 'CompareBaseline')) {
        if ($PSBoundParameters.ContainsKey($name)) { throw "-$name requires -CompareRef." }
    }
}
function Resolve-SmokeInputPath([string]$path, [string]$label, [string]$pathType) {
    if ([string]::IsNullOrWhiteSpace($path)) { throw "-$label must be a non-empty path." }
    $full = if ([System.IO.Path]::IsPathRooted($path)) { $path } else { Join-Path $RepoRoot $path }
    if (-not (Test-Path -LiteralPath $full -PathType $pathType)) { throw "-$label not found: $full" }
    $resolved = (Resolve-Path -LiteralPath $full).Path
    # Preserve a volume root such as C:\, but remove a trailing slash from other paths.
    if ($resolved.Length -gt [System.IO.Path]::GetPathRoot($resolved).Length) {
        $resolved = $resolved.TrimEnd('\')
    }
    return $resolved
}
if ($hasCompareRef) {
    $CompareRef = Resolve-SmokeInputPath $CompareRef 'CompareRef' 'Container'
    if ($PSBoundParameters.ContainsKey('CompareBaseline')) {
        $CompareBaseline = Resolve-SmokeInputPath $CompareBaseline 'CompareBaseline' 'Leaf'
    } else {
        $defaultBaseline = Join-Path $RepoRoot 'diagnostics\golden\baseline.json'
        if (Test-Path -LiteralPath $defaultBaseline -PathType Leaf) {
            $CompareBaseline = Resolve-SmokeInputPath $defaultBaseline 'CompareBaseline' 'Leaf'
        }
    }
}
$BuildDir = if ($Preset -eq "x64-base-debug") { Join-Path $RepoRoot "build\x64-debug" } else { Join-Path $RepoRoot "build\x64-release" }

# Surface local OpenVINO C runtime + Intel NPU plugin if installed in project .venv
$VenvOvLibs = Join-Path $RepoRoot ".venv\Lib\site-packages\openvino\libs"
if (Test-Path $VenvOvLibs) {
    $env:PATH = "$VenvOvLibs;$env:PATH"
}

& (Join-Path $PSScriptRoot "build.ps1") -Preset $Preset

$ExePath   = Join-Path $BuildDir "boo_sah_sah.exe"
$BenchPath = Join-Path $BuildDir "boo_bench.exe"
$DiagDir   = Join-Path $BuildDir "diagnostics"
New-Item -ItemType Directory -Force -Path $DiagDir | Out-Null

Write-Host "`n==> [1/8] Running CTest Unit, Cue Validation, and Hardware Integration Suites" -ForegroundColor Cyan
Push-Location $BuildDir
try {
    # Run unit, cue validation, and main hardware integration suite (excluding heavy 1440p PNG export tests, which have dedicated gates)
    ctest -E "BooTrilogyCaptures|BooFilmCaptures|BooInteractiveCaptures|BooFilmStress" --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw "CTest suite reported failures." }
} finally {
    Pop-Location
}

if ($hasCompareRef) {
    # Match capture_baseline.ps1's default suite names and directory layout.
    $captureRoot = Join-Path $DiagDir ("smoke_captures_" + [Guid]::NewGuid().ToString('N'))
    $integrationExe = Join-Path $BuildDir "boo_integration_tests.exe"
    $captureSuites = [ordered]@{
        film = '--film-captures'
        interactive = '--interactive-captures'
        effects = '--effects-captures'
    }
    Push-Location $RepoRoot
    try {
        foreach ($suite in $captureSuites.Keys) {
            $captureDir = Join-Path $captureRoot $suite
            New-Item -ItemType Directory -Force -Path $captureDir | Out-Null
            Write-Host "`n==> Capturing $suite to $captureDir" -ForegroundColor Cyan
            $captureArgs = @($captureSuites[$suite], '--no-activate', '--capture-dir', $captureDir)
            if ($LowPowerGpu) { $captureArgs += '--low-power-gpu' }
            & $integrationExe @captureArgs | Out-Host
            if ($LASTEXITCODE -ne 0) { throw "The $suite capture suite failed with exit code $LASTEXITCODE" }
        }
    } finally {
        Pop-Location
    }

    Write-Host "`n==> Comparing smoke captures with $CompareRef" -ForegroundColor Cyan
    $compareParams = @{
        CompareTolerant = $true
        Run = $captureRoot
        Ref = $CompareRef
        Heatmaps = $true
    }
    if ($PSBoundParameters.ContainsKey('CompareMaxMean')) { $compareParams.MaxMean = $CompareMaxMean }
    if ($PSBoundParameters.ContainsKey('CompareMap')) { $compareParams.Map = $CompareMap }
    if ($CompareBaseline) { $compareParams.Baseline = $CompareBaseline }
    try {
        & (Join-Path $PSScriptRoot 'capture_baseline.ps1') @compareParams
        $compareExitCode = $LASTEXITCODE
        if ($compareExitCode -ne 0) { throw "Capture comparison failed with exit code $compareExitCode" }
        Write-Host "[PASS] Capture comparison passed. Capture folder: $captureRoot" -ForegroundColor Green
    } catch {
        Write-Host "[FAIL] Capture comparison failed. Capture folder: $captureRoot" -ForegroundColor Red
        throw
    }
}

Write-Host "`n==> [2/8] Auditing PE Import Table (Zero Optional SDK Dependency Gate)" -ForegroundColor Cyan
$DumpbinCmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -ExpandProperty Source -First 1
if (-not $DumpbinCmd) {
    $DumpbinCmd = Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if (-not $DumpbinCmd) {
    throw "Unable to locate MSVC dumpbin.exe for PE import audit."
}
$DepsOutput = (& $DumpbinCmd /dependents $ExePath) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "dumpbin dependency audit failed." }
Write-Host $DepsOutput
$ForbiddenImports = @("nvcuda", "cudart", "cublas", "openvino", "nvEncodeAPI", "Spout", "mfreadwrite", "assimp", "gltf", "fbx", "usd", "NRD", "xinput")
foreach ($forbidden in $ForbiddenImports) {
    if ($DepsOutput -match $forbidden) {
        throw "Forbidden hard DLL dependency '$forbidden' found in boo_sah_sah.exe import table!"
    }
}
Write-Host "[PASS] Verified zero hard link dependencies on CUDA, cuBLASLt, OpenVINO, NVENC, Spout, or mfreadwrite." -ForegroundColor Green

Write-Host "`n==> [3/8] Live M10 Default Full-Film (360s) + Spout2 Clean-Feed Smoke Run ($SmokeDurationSeconds s, 1920x1080)" -ForegroundColor Cyan
$DiagNormal = Join-Path $DiagDir "smoke_m10_fullfilm.json"
& $ExePath --no-activate --width 1920 --height 1080 --mute --enable-spout --spout-name BooSmoke_Clean --duration-seconds $SmokeDurationSeconds --export-diagnostics $DiagNormal | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Default M10 full-film smoke run exited with code $LASTEXITCODE" }
$NormalJson = Get-Content $DiagNormal -Raw | ConvertFrom-Json
if ($NormalJson.milestone -ne "M10") {
    throw "Expected milestone == 'M10', got '$($NormalJson.milestone)'"
}
if ($NormalJson.runtime.rendered_frames -lt 10) {
    throw "Expected rendered_frames >= 10, got $($NormalJson.runtime.rendered_frames)"
}
if ($NormalJson.runtime.swapchain_bits -notin @(8, 10)) {
    throw "Expected swapchain_bits in @(8, 10), got $($NormalJson.runtime.swapchain_bits)"
}
if ($null -eq $NormalJson.runtime.gpu_pass_ms -or $NormalJson.runtime.gpu_pass_ms.total_ms -lt 0.0) {
    throw "Expected valid gpu_pass_ms telemetry in diagnostics JSON!"
}
Write-Host "[PASS] M10 full-film + Spout2 smoke run presented $($NormalJson.runtime.rendered_frames) frames ($($NormalJson.runtime.swapchain_bits)-bit swapchain, Spout frames=$($NormalJson.runtime.spout_published_frames), GPU p50=$($NormalJson.runtime.gpu_frametime_ms.p50_ms) ms)." -ForegroundColor Green

Write-Host "`n==> [4/8] 16:10 Letterbox + D3D12 Debug Layer + Reduced-Flash & Reduced-Motion Smoke Run (1280x800)" -ForegroundColor Cyan
$DiagLetterbox = Join-Path $DiagDir "smoke_16x10_letterbox.json"
& $ExePath --no-activate --width 1280 --height 800 --mute --d3d-debug --reduced-flash --reduced-motion --duration-seconds 2.5 --export-diagnostics $DiagLetterbox | Out-Host
if ($LASTEXITCODE -ne 0) { throw "16:10 letterbox + D3D12 debug smoke run exited with code $LASTEXITCODE" }
$LbJson = Get-Content $DiagLetterbox -Raw | ConvertFrom-Json
if ($LbJson.runtime.rendered_frames -lt 10 -or $LbJson.runtime.effect_fault_count -ne 0) {
    throw "16:10 letterbox run failed validation (frames=$($LbJson.runtime.rendered_frames), faults=$($LbJson.runtime.effect_fault_count))"
}
Write-Host "[PASS] 16:10 letterbox + D3D12 debug run presented $($LbJson.runtime.rendered_frames) frames with 0 runtime faults." -ForegroundColor Green

Write-Host "`n==> [5/8] All-Fallbacks Fault-Injection Smoke Run (No RT/CUDA/Tensor/NPU/NVENC + Media Fault + Silent Clock + Safe Shader)" -ForegroundColor Cyan
$DiagFallback = Join-Path $DiagDir "smoke_all_fallbacks.json"
& $ExePath --no-activate --width 1280 --height 720 --force-silent-clock --inject-shader-fail --inject-media-fail `
    --disable-rt --disable-cuda --disable-tensor --disable-npu --disable-nvenc `
    --duration-seconds 2.5 --export-diagnostics $DiagFallback | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Fault-injection smoke run exited with code $LASTEXITCODE" }
$FbJson = Get-Content $DiagFallback -Raw | ConvertFrom-Json
if (-not $FbJson.runtime.used_embedded_fallback_shader) {
    throw "Expected embedded fallback shader to be active under --inject-shader-fail!"
}
Write-Host "[PASS] Fault-injection run presented $($FbJson.runtime.rendered_frames) frames using embedded safe shader + silent monotonic clock." -ForegroundColor Green

Write-Host "`n==> Testing the application's single-attempt device recovery with Spout active" -ForegroundColor Cyan
$DiagRecovery = Join-Path $DiagDir "smoke_device_recovery.json"
& $ExePath --no-activate --mute --width 1280 --height 720 --start-seconds 275 --duration-seconds 4.5 `
    --enable-spout --spout-name BooRecovery_Clean --inject-device-lost --export-diagnostics $DiagRecovery | Out-Host
if ($LASTEXITCODE -ne 0) { throw "Injected device recovery exited with code $LASTEXITCODE" }
$RecoveryJson = Get-Content $DiagRecovery -Raw | ConvertFrom-Json
if (-not $RecoveryJson.runtime.device_lost_injected -or $RecoveryJson.runtime.device_recovery_attempts -ne 1 -or
    $RecoveryJson.runtime.device_recovery_successes -ne 1 -or $RecoveryJson.runtime.graphics_unavailable -or
    $RecoveryJson.runtime.rendered_frames -lt 3 -or $RecoveryJson.runtime.final_sample_48k -lt (275 * 48000)) {
    throw "Device recovery or timeline preservation failed; inspect $DiagRecovery"
}
Write-Host "[PASS] One device recovery completed with timeline preserved and Spout enabled." -ForegroundColor Green

Write-Host "`n==> [6/8] Milestone M2 (180s Trilogy) & M1 (60s Taipei) Backward-Compatibility Smoke Runs" -ForegroundColor Cyan
$DiagM2Compat = Join-Path $DiagDir "smoke_m2_compat.json"
& $ExePath --no-activate --width 1280 --height 720 --mute --cue assets/cues/m2_trilogy_180s.json --duration-seconds 2.0 --export-diagnostics $DiagM2Compat | Out-Host
if ($LASTEXITCODE -ne 0) { throw "M2 trilogy compatibility smoke run exited with code $LASTEXITCODE" }
$M2Json = Get-Content $DiagM2Compat -Raw | ConvertFrom-Json
if ($M2Json.milestone -ne "M2" -or $M2Json.runtime.rendered_frames -lt 10) {
    throw "M2 compatibility smoke failed milestone/frame check!"
}

$DiagM1Compat = Join-Path $DiagDir "smoke_m1_compat.json"
& $ExePath --no-activate --width 1280 --height 720 --mute --cue assets/cues/m1_taipei_60s.json --duration-seconds 2.0 --export-diagnostics $DiagM1Compat | Out-Host
if ($LASTEXITCODE -ne 0) { throw "M1 compatibility smoke failed" }
Write-Host "[PASS] M2 (180s) and M1 (60s) cue backward-compatibility runs passed." -ForegroundColor Green

Write-Host "`n==> [7/8] Hardware Solo Cues Smoke Run (Madame N NPU Solo)" -ForegroundColor Cyan
$DiagNpuSolo = Join-Path $DiagDir "smoke_solo_npu.json"
& $ExePath --no-activate --width 1280 --height 720 --mute --cue assets/cues/solo_npu.json --duration-seconds 2.0 --export-diagnostics $DiagNpuSolo | Out-Host
if ($LASTEXITCODE -ne 0) { throw "NPU Solo smoke run exited with code $LASTEXITCODE" }
Write-Host "[PASS] Standalone NPU Solo cue run completed cleanly." -ForegroundColor Green

if (-not $SkipBenchmarks) {
    Write-Host "`n==> [8/8] Running Ad-Hoc Numerical Accelerator Benchmarks (RT, CUDA, Tensor, NVENC + HLSL Substitutes)" -ForegroundColor Cyan
    $BenchJson = Join-Path $DiagDir "benchmarks_latest.json"
    $BenchCsv  = Join-Path $DiagDir "benchmarks_latest.csv"
    & $BenchPath --iterations 30 --json $BenchJson --csv $BenchCsv
    if ($LASTEXITCODE -ne 0) { throw "boo_bench exited with code $LASTEXITCODE" }
}

Write-Host "`nAll Milestone M0-M10 automated build, validation, dependency, letterbox, fallback, solo, Spout2, media-safety, and benchmark gates PASSED." -ForegroundColor Green
