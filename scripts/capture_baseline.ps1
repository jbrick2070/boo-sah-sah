<#
.SYNOPSIS
Golden capture hashes for refactors that must not change the picture.

.DESCRIPTION
The integration capture suites render the same frames byte for byte on one machine and driver
(seeking to a sample gives the same bytes as playing to it). Hashing their PNGs before and after a
refactor shows whether any pixel changed.

Run the suites twice and record a baseline from both runs. Files that differ between two identical
runs are marked volatile and never fail a comparison. Then compare later runs against the baseline:

  ./scripts/capture_baseline.ps1 -Capture -Out diagnostics/golden/run-a
  ./scripts/capture_baseline.ps1 -Capture -Out diagnostics/golden/run-b
  ./scripts/capture_baseline.ps1 -Record -Runs diagnostics/golden/run-a, diagnostics/golden/run-b -Baseline diagnostics/golden/baseline.json
  ./scripts/capture_baseline.ps1 -Capture -Out diagnostics/golden/after
  ./scripts/capture_baseline.ps1 -Compare -Run diagnostics/golden/after -Baseline diagnostics/golden/baseline.json

For cross-GPU comparisons, compare pixels against a reference capture folder instead:

  ./scripts/capture_baseline.ps1 -CompareTolerant -Run diagnostics/golden/after -Ref diagnostics/golden/run-a -Baseline diagnostics/golden/baseline.json -Heatmaps

Compare exits 1 when a stable file changed or is missing. Hashes are only comparable on the same GPU
and driver, so the baseline records both, plus a fingerprint of the source tree it was made from.

From cmd or Git Bash (powershell -File ...), pass -Runs and -Suites as one comma-separated string,
for example -Runs "diagnostics/golden/run-a,diagnostics/golden/run-b".
#>
[CmdletBinding(DefaultParameterSetName = 'Compare')]
param(
    [Parameter(ParameterSetName = 'Capture', Mandatory = $true)][switch]$Capture,
    [Parameter(ParameterSetName = 'Capture', Mandatory = $true)][string]$Out,
    [Parameter(ParameterSetName = 'Capture')][string[]]$Suites = @('film', 'interactive', 'effects'),
    [Parameter(ParameterSetName = 'Capture')][string]$Exe = 'build\x64-release\boo_integration_tests.exe',
    [Parameter(ParameterSetName = 'Capture')][switch]$LowPowerGpu,

    [Parameter(ParameterSetName = 'Record', Mandatory = $true)][switch]$Record,
    [Parameter(ParameterSetName = 'Record', Mandatory = $true)][string[]]$Runs,

    [Parameter(ParameterSetName = 'Compare', Mandatory = $true)][switch]$Compare,
    [Parameter(ParameterSetName = 'CompareTolerant', Mandatory = $true)][switch]$CompareTolerant,
    [Parameter(ParameterSetName = 'Compare', Mandatory = $true)]
    [Parameter(ParameterSetName = 'CompareTolerant', Mandatory = $true)][string]$Run,
    [Parameter(ParameterSetName = 'CompareTolerant', Mandatory = $true)][string]$Ref,
    [Parameter(ParameterSetName = 'CompareTolerant')][double]$MaxMean,
    [Parameter(ParameterSetName = 'CompareTolerant')][double]$MaxOver8,
    [Parameter(ParameterSetName = 'CompareTolerant')][double]$MaxOver64,
    [Parameter(ParameterSetName = 'CompareTolerant')][string]$Json,
    [Parameter(ParameterSetName = 'CompareTolerant')][string[]]$Only,
    [Parameter(ParameterSetName = 'CompareTolerant')][string[]]$Map,
    [Parameter(ParameterSetName = 'CompareTolerant')][string[]]$Reject,
    [Parameter(ParameterSetName = 'CompareTolerant')][switch]$Heatmaps,

    [Parameter(ParameterSetName = 'Record', Mandatory = $true)]
    [Parameter(ParameterSetName = 'Compare', Mandatory = $true)]
    [Parameter(ParameterSetName = 'CompareTolerant')][AllowEmptyString()][string]$Baseline
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot

# powershell -File cannot pass arrays, so also accept one comma-separated string.
function Split-List([string[]]$values) {
    return @($values | ForEach-Object { $_ -split ',' } | ForEach-Object { $_.Trim() } | Where-Object { $_ })
}
if ($Runs) { $Runs = Split-List $Runs }
if ($Suites) { $Suites = Split-List $Suites }
$suiteFlags = [ordered]@{
    film        = '--film-captures'
    interactive = '--interactive-captures'
    effects     = '--effects-captures'
    trilogy     = '--trilogy-captures'
    validation  = '--validation-captures'
    director    = '--director-captures'
}

function Resolve-RepoPath([string]$path) {
    if ([System.IO.Path]::IsPathRooted($path)) { return $path }
    return (Join-Path $repo $path)
}

function Get-CaptureHashes([string]$dir) {
    $full = Resolve-RepoPath $dir
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw "Capture folder not found: $full" }
    $root = (Resolve-Path -LiteralPath $full).Path.TrimEnd('\') + '\'
    $map = @{}
    $captureFiles = @(Get-ChildItem -LiteralPath $full -File) + @(
        Get-ChildItem -LiteralPath $full -Directory |
            Where-Object { $_.Name -ne '_diff' } |
            ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Recurse -File }
    )
    $captureFiles |
        Where-Object { $_.Extension -eq '.png' -or $_.Extension -eq '.bmp' } |
        ForEach-Object {
            $relative = $_.FullName.Substring($root.Length).Replace('\', '/')
            $map[$relative] = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    if ($map.Count -eq 0) { throw "No PNG or BMP files under $full" }
    return $map
}

function Get-RunAdapters([string]$dir) {
    $names = @()
    Get-ChildItem -LiteralPath (Resolve-RepoPath $dir) -Recurse -File -Filter 'manifest.json' -ErrorAction SilentlyContinue |
        ForEach-Object {
            try {
                $adapter = (Get-Content -Raw -LiteralPath $_.FullName | ConvertFrom-Json).adapter
                if ($adapter) { $names += [string]$adapter }
            } catch { }
        }
    return @($names | Sort-Object -Unique)
}

function Get-Machine {
    try {
        return @(Get-CimInstance Win32_VideoController | Sort-Object Name |
            ForEach-Object { [ordered]@{ gpu = $_.Name; driver = $_.DriverVersion } })
    } catch { return @() }
}

function Get-SourceFingerprint {
    # Content hash of every tracked or untracked file that can change a frame, so a stale baseline is visible.
    try {
        $paths = @('src', 'include', 'assets', 'config', 'tests', 'CMakeLists.txt', 'CMakePresets.json')
        $head = (& git -C $repo rev-parse HEAD).Trim()
        $files = @(& git -C $repo ls-files --cached --others --exclude-standard -- $paths) | Sort-Object -Unique
        $lines = New-Object System.Collections.Generic.List[string]
        foreach ($file in $files) {
            $full = Join-Path $repo $file
            if (Test-Path -LiteralPath $full -PathType Leaf) {
                $lines.Add($file + ' ' + (Get-FileHash -LiteralPath $full -Algorithm SHA256).Hash)
            }
        }
        $sha = [System.Security.Cryptography.SHA256]::Create()
        $bytes = [System.Text.Encoding]::UTF8.GetBytes(($lines -join "`n"))
        $tree = -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') })
        return [ordered]@{ head = $head; files = $lines.Count; tree_sha256 = $tree }
    } catch {
        Write-Warning "Could not fingerprint the source tree: $($_.Exception.Message)"
        return $null
    }
}

if ($CompareTolerant) {
    foreach ($name in @('Baseline', 'Json')) {
        if ($PSBoundParameters.ContainsKey($name) -and [string]::IsNullOrWhiteSpace([string]$PSBoundParameters[$name])) {
            [Console]::Error.WriteLine("-$name must be a non-empty file path.")
            exit 2
        }
    }
    if ($PSBoundParameters.ContainsKey('Reject')) {
        if (-not $Reject -or @($Reject | Where-Object { [string]::IsNullOrWhiteSpace($_) }).Count) {
            [Console]::Error.WriteLine('-Reject must contain non-empty file paths.')
            exit 2
        }
    }
    $Only = Split-List $Only
    $Map = Split-List $Map

    $candidates = @()
    $venvPython = Join-Path $repo '.venv\Scripts\python.exe'
    if (Test-Path -LiteralPath $venvPython -PathType Leaf) {
        $candidates += @{ Path = $venvPython; Prefix = @() }
    }
    foreach ($command in @(Get-Command python -CommandType Application -All -ErrorAction SilentlyContinue)) {
        if ($command.Source -notmatch '(?i)[\\/]Microsoft[\\/]WindowsApps[\\/]') {
            $candidates += @{ Path = $command.Source; Prefix = @() }
        }
    }
    foreach ($command in @(Get-Command py -CommandType Application -All -ErrorAction SilentlyContinue)) {
        if ($command.Source -notmatch '(?i)[\\/]Microsoft[\\/]WindowsApps[\\/]') {
            $candidates += @{ Path = $command.Source; Prefix = @('-3') }
        }
    }
    $python = $null
    foreach ($candidate in $candidates) {
        try {
            $previousErrorActionPreference = $ErrorActionPreference
            $ErrorActionPreference = 'Continue'
            $version = & $candidate.Path @($candidate.Prefix) --version 2>&1
            if ($LASTEXITCODE -eq 0 -and "$version" -match '^Python 3\.') { $python = $candidate; break }
        } catch { } finally {
            $ErrorActionPreference = $previousErrorActionPreference
        }
    }
    if (-not $python) {
        [Console]::Error.WriteLine('No working Python 3 interpreter found in .venv, on PATH, or via py -3; -CompareTolerant requires Python 3.')
        exit 2
    }
    $arguments = @((Join-Path $repo 'tools/compare_captures.py'), '--run', (Resolve-RepoPath $Run), '--ref', (Resolve-RepoPath $Ref))
    if ($PSBoundParameters.ContainsKey('Baseline')) { $arguments += @('--baseline', (Resolve-RepoPath $Baseline)) }
    if ($PSBoundParameters.ContainsKey('MaxMean')) { $arguments += @('--max-mean', $MaxMean.ToString([System.Globalization.CultureInfo]::InvariantCulture)) }
    if ($PSBoundParameters.ContainsKey('MaxOver8')) { $arguments += @('--max-over8', $MaxOver8.ToString([System.Globalization.CultureInfo]::InvariantCulture)) }
    if ($PSBoundParameters.ContainsKey('MaxOver64')) { $arguments += @('--max-over64', $MaxOver64.ToString([System.Globalization.CultureInfo]::InvariantCulture)) }
    if ($PSBoundParameters.ContainsKey('Json')) { $arguments += @('--json', (Resolve-RepoPath $Json)) }
    foreach ($pattern in $Only) { $arguments += @('--only', $pattern) }
    foreach ($mapping in $Map) { $arguments += @('--map', $mapping) }
    foreach ($image in $Reject) { $arguments += @('--reject', (Resolve-RepoPath $image)) }
    if ($Heatmaps) { $arguments += '--heatmaps' }
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        $ErrorActionPreference = 'Continue'
        & $python.Path @($python.Prefix) @arguments
        $code = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    exit $code
}

if ($Capture) {
    $exePath = Resolve-RepoPath $Exe
    if (-not (Test-Path -LiteralPath $exePath -PathType Leaf)) { throw "Not found: $exePath (build Release first)" }
    $outRoot = Resolve-RepoPath $Out
    if (Test-Path -LiteralPath $outRoot) { throw "Output folder already exists, pick a new one: $outRoot" }
    foreach ($suite in $Suites) {
        $flag = $suiteFlags[$suite]
        if (-not $flag) { throw "Unknown suite '$suite'. Known suites: $($suiteFlags.Keys -join ', ')" }
        $dir = Join-Path $outRoot $suite
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        Write-Host "==> $suite ($flag) -> $dir"
        $start = Get-Date
        Push-Location $repo
        try {
            $captureArgs = @($flag, '--no-activate', '--capture-dir', $dir)
            if ($LowPowerGpu) { $captureArgs += '--low-power-gpu' }
            & $exePath @captureArgs | Out-Host
            $code = $LASTEXITCODE
        } finally { Pop-Location }
        Write-Host ('    exit {0} after {1:n0} s' -f $code, ((Get-Date) - $start).TotalSeconds)
        if ($code -ne 0) { throw "The $suite suite failed with exit code $code" }
    }
    exit 0
}

if ($Record) {
    $runHashes = @($Runs | ForEach-Object { , (Get-CaptureHashes $_) })
    $allNames = @($runHashes | ForEach-Object { $_.Keys }) | Sort-Object -Unique
    $files = [ordered]@{}
    $stable = 0
    $volatile = 0
    foreach ($name in $allNames) {
        $first = $runHashes[0][$name]
        $same = $true
        foreach ($hashes in $runHashes) {
            if (-not $hashes.ContainsKey($name) -or $hashes[$name] -ne $first) { $same = $false }
        }
        if ($same) { $stable++ } else { $volatile++ }
        $files[$name] = [ordered]@{ sha256 = $first; stable = $same }
    }
    $adapters = @($Runs | ForEach-Object { Get-RunAdapters $_ }) | Sort-Object -Unique
    $document = [ordered]@{
        schema   = 1
        created  = (Get-Date).ToString('o')
        runs     = @($Runs)
        adapters = @($adapters)
        machine  = @(Get-Machine)
        source   = Get-SourceFingerprint
        files    = $files
    }
    $baselinePath = Resolve-RepoPath $Baseline
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $baselinePath) | Out-Null
    $document | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $baselinePath -Encoding UTF8
    Write-Host "Recorded $($files.Count) files from $($Runs.Count) run(s): $stable stable, $volatile volatile."
    Write-Host "Baseline: $baselinePath"
    if ($Runs.Count -lt 2) { Write-Warning 'One run cannot tell stable files from volatile ones. Record from two runs.' }
    exit 0
}

if ($Compare) {
    $baselinePath = Resolve-RepoPath $Baseline
    if (-not (Test-Path -LiteralPath $baselinePath -PathType Leaf)) { throw "Baseline not found: $baselinePath" }
    $base = Get-Content -Raw -LiteralPath $baselinePath | ConvertFrom-Json
    $now = Get-CaptureHashes $Run

    $baseMachine = (@($base.machine) | ForEach-Object { "$($_.gpu) $($_.driver)" }) -join '; '
    $nowMachine = (@(Get-Machine) | ForEach-Object { "$($_.gpu) $($_.driver)" }) -join '; '
    if ($baseMachine -and $nowMachine -and $baseMachine -ne $nowMachine) {
        Write-Warning "GPU or driver changed since the baseline ($baseMachine -> $nowMachine). Hashes are only comparable on the same GPU and driver."
    }
    $source = Get-SourceFingerprint
    if ($base.source -and $source) {
        if ($base.source.tree_sha256 -eq $source.tree_sha256) { Write-Host 'Source tree is identical to the baseline.' }
        else { Write-Host 'Source tree differs from the baseline (expected after a refactor).' }
    }

    $matched = 0
    $volatileDiffers = 0
    $changed = New-Object System.Collections.Generic.List[string]
    $missing = New-Object System.Collections.Generic.List[string]
    $baseNames = New-Object System.Collections.Generic.HashSet[string]
    foreach ($property in $base.files.PSObject.Properties) {
        $name = $property.Name
        $entry = $property.Value
        [void]$baseNames.Add($name)
        if (-not $now.ContainsKey($name)) {
            if ($entry.stable) { $missing.Add($name) }
            continue
        }
        if ($now[$name] -eq $entry.sha256) { $matched++ }
        elseif ($entry.stable) { $changed.Add($name) }
        else { $volatileDiffers++ }
    }
    $added = @($now.Keys | Where-Object { -not $baseNames.Contains($_) } | Sort-Object)

    Write-Host "Matched: $matched. Volatile files that differ (ignored): $volatileDiffers."
    if ($added.Count) { Write-Host "Not in the baseline: $($added.Count)"; $added | ForEach-Object { Write-Host "  + $_" } }
    if ($missing.Count) { Write-Host "Missing stable files: $($missing.Count)"; $missing | ForEach-Object { Write-Host "  - $_" } }
    if ($changed.Count) { Write-Host "Changed stable files: $($changed.Count)"; $changed | ForEach-Object { Write-Host "  ! $_" } }
    if ($changed.Count -or $missing.Count) { exit 1 }
    Write-Host 'No stable capture changed.'
    exit 0
}
