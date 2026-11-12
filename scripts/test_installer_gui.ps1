param(
    [Parameter(Mandatory = $true)]
    [string]$SetupPath,
    [ValidateRange(5, 300)]
    [int]$TimeoutSeconds = 60
)

$ErrorActionPreference = 'Stop'
$SetupPath = (Resolve-Path -LiteralPath $SetupPath).Path
if ([IO.Path]::GetExtension($SetupPath) -ine '.exe') { throw 'SetupPath must be a packed installer executable.' }

$NativeSource = @'
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace BooInstallerGuiProbe {
    public static class Native {
        private delegate bool EnumWindowsProc(IntPtr window, IntPtr parameter);
        [DllImport("user32.dll")]
        private static extern bool EnumWindows(EnumWindowsProc callback, IntPtr parameter);
        [DllImport("user32.dll")]
        private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int GetClassNameW(IntPtr window, StringBuilder name, int count);
        [DllImport("user32.dll")]
        public static extern IntPtr GetDlgItem(IntPtr window, int id);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string className, string title);
        [DllImport("user32.dll")]
        public static extern IntPtr GetWindowLongPtrW(IntPtr window, int index);
        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool IsWindowEnabled(IntPtr window);
        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool IsWindowVisible(IntPtr window);
        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool ShowWindow(IntPtr window, int command);
        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        public static extern bool PostMessageW(IntPtr window, uint message, UIntPtr wParam, IntPtr lParam);
        [DllImport("user32.dll", SetLastError = true)]
        public static extern IntPtr SendMessageTimeoutW(IntPtr window, uint message, UIntPtr wParam,
            IntPtr lParam, uint flags, uint timeout, out UIntPtr result);

        public static IntPtr FindSetupWindow(uint processId) {
            IntPtr found = IntPtr.Zero;
            EnumWindows((window, parameter) => {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner != processId) return true;
                var name = new StringBuilder(128);
                GetClassNameW(window, name, name.Capacity);
                if (name.ToString() != "BooSahSahSetupWnd") return true;
                found = window;
                return false;
            }, IntPtr.Zero);
            return found;
        }
        public static UIntPtr SendChecked(IntPtr window, uint message, UIntPtr wParam) {
            UIntPtr result;
            // SMTO_BLOCK | SMTO_ABORTIFHUNG, bounded to one second.
            if (window == IntPtr.Zero || SendMessageTimeoutW(window, message, wParam,
                    IntPtr.Zero, 3, 1000, out result) == IntPtr.Zero)
                throw new InvalidOperationException("Setup UI did not respond within one second.");
            return result;
        }
    }
}
'@
if (-not ('BooInstallerGuiProbe.Native' -as [type])) { Add-Type -TypeDefinition $NativeSource }

$TempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$ProbeRoot = [IO.Path]::GetFullPath((Join-Path $TempRoot ('boo-gui-probe-' + [guid]::NewGuid().ToString('N'))))
$InstallDir = Join-Path $ProbeRoot 'app space & 100% ! (cancel)'
$Process = $null
$Window = [IntPtr]::Zero
$Succeeded = $false

try {
    # The installer currently rejects any running app with this basename.
    if (Get-Process boo_sah_sah -ErrorAction SilentlyContinue) {
        throw 'Close the running BOO SAH SAH app before testing installer cancellation.'
    }
    [IO.Directory]::CreateDirectory($ProbeRoot) | Out-Null
    # Request hidden startup directly and pass --no-activate as a second guard.
    $StartInfo = [Diagnostics.ProcessStartInfo]::new()
    $StartInfo.FileName = $SetupPath
    $StartInfo.UseShellExecute = $false
    $StartInfo.CreateNoWindow = $true
    $StartInfo.WindowStyle = [Diagnostics.ProcessWindowStyle]::Hidden
    foreach ($Argument in @('--install-dir', $InstallDir, '--no-shortcuts', '--no-registry', '--no-launch', '--no-activate')) {
        $StartInfo.ArgumentList.Add($Argument)
    }
    $Process = [Diagnostics.Process]::Start($StartInfo)
    $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        $Window = [BooInstallerGuiProbe.Native]::FindSetupWindow([uint32]$Process.Id)
        if ($Window -ne [IntPtr]::Zero) { break }
        if ($Process.HasExited) { throw "Setup exited before creating its window ($($Process.ExitCode))." }
        Start-Sleep -Milliseconds 10
    } while ([DateTime]::UtcNow -lt $Deadline)
    if ($Window -eq [IntPtr]::Zero) { throw 'Timed out locating the hidden setup window.' }
    if (-not $Process.WaitForInputIdle(5000)) { throw 'Setup did not finish initializing its GUI.' }
    # Setup explicitly calls ShowWindow during initialization. Hide this exact
    # PID-owned window before exercising any controls, even if it overrides SW_HIDE.
    [BooInstallerGuiProbe.Native]::ShowWindow($Window, 0) | Out-Null
    if ([BooInstallerGuiProbe.Native]::IsWindowVisible($Window)) { throw 'Could not hide the probe setup window.' }

    # GUI defaults are checked even when command-line shortcut/launch flags are off.
    foreach ($Id in @(102, 103, 104)) {
        $Control = [BooInstallerGuiProbe.Native]::GetDlgItem($Window, $Id)
        [BooInstallerGuiProbe.Native]::SendChecked($Control, 0xF1, [UIntPtr]::Zero) | Out-Null # BM_SETCHECK / BST_UNCHECKED
        if ([BooInstallerGuiProbe.Native]::SendChecked($Control, 0xF0, [UIntPtr]::Zero).ToUInt64() -ne 0) {
            throw "Could not clear setup checkbox $Id."
        }
    }
    $InstallButton = [BooInstallerGuiProbe.Native]::GetDlgItem($Window, 106)
    $CancelButton = [BooInstallerGuiProbe.Native]::GetDlgItem($Window, 107)
    if (-not [BooInstallerGuiProbe.Native]::PostMessageW($Window, 0x111, [UIntPtr]106, [IntPtr]::Zero)) {
        throw 'Could not post the Install command.'
    }
    $Deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    while ([BooInstallerGuiProbe.Native]::IsWindowEnabled($InstallButton)) {
        if ($Process.HasExited) { throw "Setup exited before worker verification ($($Process.ExitCode))." }
        if ([DateTime]::UtcNow -ge $Deadline) { throw 'Install command did not start the worker.' }
        Start-Sleep -Milliseconds 1
    }
    if (-not [BooInstallerGuiProbe.Native]::IsWindowEnabled($CancelButton)) {
        throw 'Setup did not leave Cancel available during installation.'
    }
    $ProgressBar = [BooInstallerGuiProbe.Native]::FindWindowExW($Window, [IntPtr]::Zero, 'msctls_progress32', $null)
    if ($ProgressBar -eq [IntPtr]::Zero) { throw 'Could not locate the setup progress control.' }
    # PBS_MARQUEE is enabled by a progress message from the worker after its first
    # cancellation checkpoint. Observe it before probing, so an unstarted worker
    # cannot produce a misleading responsiveness/cancellation pass.
    while (([BooInstallerGuiProbe.Native]::GetWindowLongPtrW($ProgressBar, -16).ToInt64() -band 8) -eq 0) {
        if ($Process.HasExited) { throw "Setup finished before observing decompression ($($Process.ExitCode)); use a full packed installer." }
        if ([DateTime]::UtcNow -ge $Deadline) { throw 'No worker decompression progress was observed.' }
        Start-Sleep -Milliseconds 1
    }
    $ResponseTimer = [Diagnostics.Stopwatch]::StartNew()
    [BooInstallerGuiProbe.Native]::SendChecked($Window, 0, [UIntPtr]::Zero) | Out-Null # WM_NULL
    $ResponseTimer.Stop()
    if ($Process.HasExited -or [BooInstallerGuiProbe.Native]::IsWindowEnabled($InstallButton)) {
        throw 'Installation finished or failed before responsiveness could be verified; use a full packed installer.'
    }
    if (-not [BooInstallerGuiProbe.Native]::PostMessageW($Window, 0x111, [UIntPtr]107, [IntPtr]::Zero)) {
        throw 'Could not post the Cancel command.'
    }
    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) { throw 'Setup did not finish cancellation within the timeout.' }
    if ($Process.ExitCode -ne 1223) { throw "Cancellation expected ERROR_CANCELLED (1223), got $($Process.ExitCode)." }
    $Remaining = @(Get-ChildItem -LiteralPath $ProbeRoot -Force)
    if ($Remaining.Count) { throw "Cancellation left installation or transaction files at $ProbeRoot." }
    $Succeeded = $true
    Write-Host "[PASS] Hidden setup worker answered WM_NULL in $($ResponseTimer.ElapsedMilliseconds) ms; Cancel returned 1223 with no installed or transaction files." -ForegroundColor Green
} finally {
    if ($null -ne $Process -and -not $Process.HasExited) {
        # Address only the process launched by this probe; request cooperative cleanup first.
        if ($Window -ne [IntPtr]::Zero) {
            [BooInstallerGuiProbe.Native]::PostMessageW($Window, 0x10, [UIntPtr]::Zero, [IntPtr]::Zero) | Out-Null # WM_CLOSE
        }
        if (-not $Process.WaitForExit(5000)) {
            Write-Warning "Probe setup process $($Process.Id) did not close; stopping only this process and retaining all artifacts."
            $Process.Kill()
            $Process.WaitForExit()
        }
    }
    if ($Succeeded) {
        if (-not $ProbeRoot.StartsWith($TempRoot, [StringComparison]::OrdinalIgnoreCase) -or
            -not [IO.Path]::GetFileName($ProbeRoot).StartsWith('boo-gui-probe-')) { throw 'Unsafe probe cleanup path.' }
        # The verified root is empty. Avoid recursive deletion even on successful probes.
        Remove-Item -LiteralPath $ProbeRoot
    } elseif (Test-Path -LiteralPath $ProbeRoot) { Write-Warning "GUI probe artifacts retained at $ProbeRoot" }
    if ($null -ne $Process) { $Process.Dispose() }
}
