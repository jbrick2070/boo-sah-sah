param(
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutDir)) {
    $OutDir = Join-Path $RepoRoot "third_party\nvidia_nrd"
}

$WorkRoot = Join-Path $RepoRoot "build\nrd_build_src"
$SrcDir   = Join-Path $WorkRoot "NRD"
$BldDir   = Join-Path $WorkRoot "build"

New-Item -ItemType Directory -Force -Path $WorkRoot | Out-Null
if (-not (Test-Path (Join-Path $SrcDir "CMakeLists.txt"))) {
    Write-Host "==> Cloning NVIDIA-RTX/NRD (shallow)..." -ForegroundColor Cyan
    git clone --depth 1 https://github.com/NVIDIA-RTX/NRD.git $SrcDir
    if ($LASTEXITCODE -ne 0) { throw "Failed to clone NVIDIA-RTX/NRD" }
}

Write-Host "==> Configuring NVIDIA NRD (DXIL only, no NRI, shared DLL)..." -ForegroundColor Cyan
cmake -S $SrcDir -B $BldDir `
    -DNRD_STATIC_LIBRARY=OFF `
    -DNRD_NRI=OFF `
    -DNRD_EMBEDS_DXIL_SHADERS=ON `
    -DNRD_EMBEDS_DXBC_SHADERS=OFF `
    -DNRD_EMBEDS_SPIRV_SHADERS=OFF `
    -DNRD_NORMAL_ENCODING=2 `
    -DNRD_ROUGHNESS_ENCODING=1
if ($LASTEXITCODE -ne 0) { throw "CMake configure for NRD failed" }

Write-Host "==> Building NRD.dll (Release)..." -ForegroundColor Cyan
cmake --build $BldDir --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "CMake build for NRD failed" }

$BuiltDll = Get-ChildItem -Path $SrcDir, $BldDir -Recurse -Filter "NRD.dll" |
    Where-Object { $_.FullName -match "Release|_Bin" } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $BuiltDll) {
    $BuiltDll = Get-ChildItem -Path $SrcDir, $BldDir -Recurse -Filter "NRD.dll" | Select-Object -First 1
}
if (-not $BuiltDll) { throw "NRD.dll not found after build!" }

New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "Include") | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $OutDir "bin") | Out-Null

Copy-Item -Force $BuiltDll.FullName (Join-Path $OutDir "bin\NRD.dll")
Copy-Item -Force (Join-Path $SrcDir "Include\*.h") (Join-Path $OutDir "Include\")
Copy-Item -Force (Join-Path $SrcDir "LICENSE.txt") (Join-Path $OutDir "LICENSE-NVIDIA-NRD.txt")

Write-Host "==> Staged NVIDIA NRD sidecar to $OutDir\bin\NRD.dll ($([math]::Round($BuiltDll.Length / 1MB, 2)) MB)" -ForegroundColor Green
