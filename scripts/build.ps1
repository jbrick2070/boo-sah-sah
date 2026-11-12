param(
    [string]$Preset = "x64-base-release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = if ($Preset -eq "x64-base-debug") { Join-Path $RepoRoot "build\x64-debug" } else { Join-Path $RepoRoot "build\x64-release" }

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    $VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $DevShellDll = Join-Path $VsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Import-Module $DevShellDll
    Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

if (-not (Test-Path (Join-Path $BuildDir "build.ninja"))) {
    & (Join-Path $PSScriptRoot "configure.ps1") -Preset $Preset
}

Write-Host "==> Building BOO SAH SAH ($Preset)" -ForegroundColor Cyan
Push-Location $RepoRoot
try {
    cmake --build --preset $Preset
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
