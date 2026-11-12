param(
    [string]$Preset = "x64-base-release"
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    $VsWhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (-not (Test-Path $VsWhere)) {
        throw "vswhere.exe not found. Please install Visual Studio 2022 or Build Tools 2022."
    }
    $VsPath = & $VsWhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $VsPath) {
        throw "No Visual Studio C++ x64 toolset installation found."
    }
    $DevShellDll = Join-Path $VsPath "Common7\Tools\Microsoft.VisualStudio.DevShell.dll"
    Import-Module $DevShellDll
    Enter-VsDevShell -VsInstallPath $VsPath -SkipAutomaticLocation -DevCmdArguments "-arch=x64 -host_arch=x64" | Out-Null
}

Write-Host "==> Configuring BOO SAH SAH ($Preset)" -ForegroundColor Cyan
Push-Location $RepoRoot
try {
    cmake --preset $Preset --fresh
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configure failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}
