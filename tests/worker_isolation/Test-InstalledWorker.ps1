# Local, side-by-side acceptance test. A fresh leaf certificate signs only this test identity.
# -AllowMachineTrust permits one Windows elevation prompt if per-user leaf trust is insufficient.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$HarnessExe,
    [string]$ViewerDirectory,
    [ValidateSet('raster','path','dlss','warp')][string[]]$Modes = @('raster','path','dlss','warp'),
    [switch]$AllowMachineTrust,
    [switch]$DiagnosticRegistration,
    [switch]$RepeatFinished
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$tag = [Guid]::NewGuid().ToString('N').Substring(0,20)
$identity = "BooSahSah.ContainmentTest.$tag"
$subject = "CN=Boo Sah Sah Containment $tag"
$work = (New-Item -ItemType Directory -Path (Join-Path $root "build\containment-$tag")).FullName
$layout = (New-Item -ItemType Directory -Path (Join-Path $work 'stage')).FullName
$package = Join-Path $work 'test.msix'
$receipt = Join-Path $root "build\containment-receipt-$tag.txt"
$sdk = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Directory |
    Where-Object { $_.Name -match '^\d+\.' -and (Test-Path (Join-Path $_.FullName 'x64\makeappx.exe')) } |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$sdk) { throw 'Windows SDK package tools are required.' }

Add-Type -TypeDefinition @"
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
[ComImport, Guid("2e941141-7f97-4756-ba1d-9decde894a3d"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
interface IContainmentActivation {
    void ActivateApplication([MarshalAs(UnmanagedType.LPWStr)] string id,
        [MarshalAs(UnmanagedType.LPWStr)] string args, uint options, out uint processId);
}
public static class ContainmentActivation {
    [DllImport("kernel32", SetLastError=true)] static extern IntPtr OpenProcess(uint access, bool inherit, uint pid);
    [DllImport("kernel32")] static extern uint WaitForSingleObject(IntPtr process, uint timeout);
    [DllImport("kernel32")] static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("kernel32")] static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32")] static extern bool CloseHandle(IntPtr process);
    public static int Run(string id, string args, int timeout) {
        object manager = Activator.CreateInstance(Type.GetTypeFromCLSID(new Guid("45BA127D-10A8-46EA-8AB7-56EA9078943C")));
        try {
            uint pid;
            ((IContainmentActivation)manager).ActivateApplication(id,args,6,out pid);
            IntPtr process = OpenProcess(0x101001,false,pid);
            if(process == IntPtr.Zero) throw new System.ComponentModel.Win32Exception();
            try {
                if(WaitForSingleObject(process,(uint)timeout) != 0) {
                    TerminateProcess(process,1);
                    WaitForSingleObject(process,2000);
                    throw new TimeoutException("Packaged test timed out.");
                }
                uint code;
                if(!GetExitCodeProcess(process,out code)) throw new System.ComponentModel.Win32Exception();
                return (int)code;
            } finally { CloseHandle(process); }
        } finally { Marshal.ReleaseComObject(manager); }
    }
}
"@
function Run-Installed($application, $arguments, $timeout) {
    $code = [ContainmentActivation]::Run(($installed.PackageFamilyName + '!' + $application), $arguments, $timeout)
    if ($code -ne 0) { throw "$application $arguments exited with $code." }
    Write-Output "PASS installed $application $arguments"
}
function Start-TemporaryMachineTrust($certificate) {
    $bin = [Convert]::ToBase64String($certificate.RawData)
    $command = @'
$ErrorActionPreference = 'Stop'
$certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new([Convert]::FromBase64String('PUBLIC_CERT'))
$ready = [Threading.EventWaitHandle]::OpenExisting('Local\BRC_READY_TAG')
$done = [Threading.EventWaitHandle]::OpenExisting('Local\BRC_DONE_TAG')
$store = [Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople','LocalMachine')
try {
    $store.Open('ReadWrite')
    if ($store.Certificates.Find('FindByThumbprint',$certificate.Thumbprint,$false).Count) { throw 'Leaf already trusted.' }
    $store.Add($certificate)
    try {
        [void]$ready.Set()
        [void]$done.WaitOne(420000)
    } finally {
        $store.Remove($certificate)
    }
} finally {
    $store.Close()
    $ready.Dispose()
    $done.Dispose()
}
'@
    $command = $command.Replace('PUBLIC_CERT',$bin).Replace('_TAG',"_$tag")
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
    Write-Host 'Windows may request administrator approval for one temporary Trusted People certificate.'
    $systemShell = Join-Path $env:SystemRoot 'System32\WindowsPowerShell\v1.0\powershell.exe'
    return Start-Process -FilePath $systemShell -Verb RunAs -WindowStyle Hidden -PassThru -ArgumentList @(
        '-NoProfile','-NonInteractive','-EncodedCommand',$encoded)
}

$cert = $null
$installed = $null
$trusted = $null
$elevated = $null
$ready = [Threading.EventWaitHandle]::new($false,'ManualReset',"Local\BRC_READY_$tag")
$done = [Threading.EventWaitHandle]::new($false,'ManualReset',"Local\BRC_DONE_$tag")
try {
    Copy-Item -LiteralPath $HarnessExe -Destination (Join-Path $layout 'containment.exe')
    $entry = 'containment.exe'
    $bytes = [IO.File]::ReadAllBytes((Join-Path $layout $entry))
    $peOffset = [BitConverter]::ToInt32($bytes,60)
    if ([BitConverter]::ToUInt16($bytes,$peOffset+24+68) -eq 3) {
        # Keep the CMake console fixture byte-for-byte identical and start it invisibly within
        # the activated package. It independently requires package identity in both processes.
        $entry = 'containment-host.exe'
        Add-Type -OutputAssembly (Join-Path $layout $entry) -OutputType WindowsApplication -TypeDefinition @"
using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
static class QuietContainmentEntry {
    static int Main(string[] args) {
        if (args.Any(a=>a.Contains("\""))) return 2;
        var exe = Path.Combine(AppDomain.CurrentDomain.BaseDirectory,"containment.exe");
        var start = new ProcessStartInfo(exe,string.Join(" ",args.Select(a=>"\""+a+"\""))) {
            UseShellExecute=false, CreateNoWindow=true, WindowStyle=ProcessWindowStyle.Hidden
        };
        using (var child=Process.Start(start)) {
            if (!child.WaitForExit(70000)) { child.Kill(); child.WaitForExit(1500); return 124; }
            return child.ExitCode;
        }
    }
}
"@
    }
    if ($ViewerDirectory) {
        $ViewerDirectory = (Resolve-Path -LiteralPath $ViewerDirectory).Path
        foreach ($required in 'boo_sah_sah.exe','config\default.toml','assets\manifest.json','assets\cues\film_360s.json') {
            if (!(Test-Path -LiteralPath (Join-Path $ViewerDirectory $required) -PathType Leaf)) { throw "Viewer staging missing $required" }
        }
        foreach ($name in @('boo_sah_sah.exe','assets','config','dlss','nrd','openvino','streamline')) {
            $source = Join-Path $ViewerDirectory $name
            if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $layout -Recurse }
        }
    }
    $payload = @{'containment.exe' = (Get-FileHash -Algorithm SHA256 -LiteralPath $HarnessExe).Hash}
    if ($entry -ne 'containment.exe') { $payload[$entry] = (Get-FileHash -LiteralPath (Join-Path $layout $entry)).Hash }
    if ($ViewerDirectory) {
        $payload['boo_sah_sah.exe'] = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $ViewerDirectory 'boo_sah_sah.exe')).Hash
    }
    foreach ($name in $payload.Keys) {
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $layout $name)).Hash -ne $payload[$name]) {
            throw "Staged binary differs from source: $name"
        }
        Write-Output "Staged $name SHA256 $($payload[$name])"
    }
    $logos = New-Item -ItemType Directory -Path (Join-Path $layout 'TestLogos')
    foreach ($name in @('StoreLogo','Square150x150Logo','Square44x44Logo')) {
        Copy-Item (Join-Path $root "packaging\msix\assets\$name.scale-100.png") (Join-Path $logos "$name.png")
    }
    $manifest = [IO.File]::ReadAllText((Join-Path $root 'packaging\msix\AppxManifest.xml')).
        Replace('$IDENTITY_NAME$',$identity).Replace('$PUBLISHER$',$subject).Replace('$VERSION$','1.0.0.0').
        Replace('$PUBLISHER_DISPLAY$','Local test').Replace('$RESOURCES$','<Resource Language="en-us"/>').
        Replace('ms-resource:AppDescription','Local benchmark containment verification').Replace('Assets\','TestLogos\')
    [xml]$xml = $manifest
    $application = $xml.Package.Applications.Application
    if ($ViewerDirectory) {
        $viewer = $application.CloneNode($true)
        $viewer.SetAttribute('Id','Viewer')
        [void]$xml.Package.Applications.AppendChild($viewer)
    }
    $application.SetAttribute('Executable',$entry)
    $xml.Save((Join-Path $layout 'AppxManifest.xml'))
    if ($DiagnosticRegistration) {
        # Repeat the timing-sensitive teardown case with package identity and no changes to signing trust.
        Add-AppxPackage -Register (Join-Path $layout 'AppxManifest.xml') -ErrorAction Stop
        $installed = Get-AppxPackage -Name $identity
        if (!$installed.IsDevelopmentMode) { throw 'Expected a loose diagnostic registration.' }
        Write-Output "Diagnostic development registration $($installed.PackageFullName)"
        Run-Installed 'App' ('--no-activate --mute --expect-package --receipt "'+$receipt+'"') 75000
        if ($RepeatFinished) {
            Run-Installed 'App' ('--no-activate --mute --expect-package --repeat-finish --receipt "'+$receipt+'.repeat"') 70000
        }
        return
    }
    & (Join-Path $sdk.FullName 'x64\makeappx.exe') pack /d $layout /p $package /h SHA256 | Out-Null
    if ($LASTEXITCODE) { throw 'MSIX pack failed.' }
    $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\CurrentUser\My -KeyExportPolicy NonExportable -NotAfter ([DateTime]::Now.AddDays(1)) -HashAlgorithm SHA256
    Write-Output "Temporary signing leaf $($cert.Subject), thumbprint $($cert.Thumbprint), CurrentUser\My, nonexportable"
    $rsa = [Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($cert)
    $keyName = $rsa.Key.KeyName
    $keyProvider = $rsa.Key.Provider
    $rsa.Dispose()
    & (Join-Path $sdk.FullName 'x64\signtool.exe') sign /fd SHA256 /sha1 $cert.Thumbprint /s My $package | Out-Null
    if ($LASTEXITCODE) { throw 'MSIX signing failed.' }
    Write-Output "Package $identity SHA256 $((Get-FileHash -Algorithm SHA256 $package).Hash)"
    $public = [Security.Cryptography.X509Certificates.X509Certificate2]::new($cert.RawData)
    $trusted = [Security.Cryptography.X509Certificates.X509Store]::new('TrustedPeople','CurrentUser')
    $trusted.Open('ReadWrite')
    $trusted.Add($public)
    try { Add-AppxPackage -Path $package -ErrorAction Stop }
    catch {
        if (!$AllowMachineTrust -or $_.Exception.Message -notmatch '0x800B0109|untrusted|not trusted') { throw }
        $elevated = Start-TemporaryMachineTrust $cert
        if (!$ready.WaitOne(15000)) { throw 'Temporary machine trust was not established.' }
        Add-AppxPackage -Path $package -ErrorAction Stop
    }
    $installed = Get-AppxPackage -Name $identity
    if (!$installed -or $installed.IsDevelopmentMode -or $installed.SignatureKind -eq 'None') {
        throw 'Expected a deployed, signed MSIX.'
    }
    Write-Output "Installed $($installed.PackageFullName), signature $($installed.SignatureKind), development mode $($installed.IsDevelopmentMode)"
    foreach ($name in $payload.Keys) {
        $installedHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $installed.InstallLocation $name)).Hash
        if ($installedHash -ne $payload[$name]) { throw "Installed binary differs from tested source: $name" }
        Write-Output "Verified installed $name SHA256 $installedHash"
    }
    Write-Output "Containment assertion receipt: $receipt"
    $failures = @()
    try { Run-Installed 'App' ('--no-activate --mute --expect-package --receipt "'+$receipt+'"') 75000 }
    catch { $failures += $_.Exception.Message; Write-Warning $_.Exception.Message }
    if ($ViewerDirectory) {
        foreach ($mode in $Modes) {
            try { Run-Installed 'Viewer' "--verify-film-benchmark $mode --no-activate --mute" 65000 }
            catch { $failures += $_.Exception.Message; Write-Warning $_.Exception.Message }
        }
    }
    if ($failures.Count) { throw ($failures -join [Environment]::NewLine) }
} finally {
    try {
        $installed = Get-AppxPackage -Name $identity
        if ($installed) { Remove-AppxPackage -Package $installed.PackageFullName -ErrorAction Stop }
    } finally {
        try {
            [void]$done.Set()
            if ($elevated -and (!$elevated.WaitForExit(15000) -or $elevated.ExitCode -ne 0)) {
                Write-Error 'The temporary machine trust helper could not confirm cleanup.' -ErrorAction Continue
            }
        } finally {
            if ($trusted) { $trusted.Remove($public); $trusted.Close() }
            if ($cert) { Remove-Item -LiteralPath ("Cert:\CurrentUser\My\"+$cert.Thumbprint) -DeleteKey -Force }
            $ready.Dispose()
            $done.Dispose()
        }
    }
    if ($cert) {
        foreach ($store in @('CurrentUser\My','CurrentUser\TrustedPeople','LocalMachine\TrustedPeople')) {
            if (Test-Path -LiteralPath "Cert:\$store\$($cert.Thumbprint)") { throw "Temporary certificate remains in $store." }
        }
        if ($keyName -and [Security.Cryptography.CngKey]::Exists($keyName,$keyProvider)) { throw 'Temporary signing key remains.' }
    }
    if (Get-AppxPackage -Name $identity) { throw 'Temporary package remains installed.' }
    $buildRoot = (Resolve-Path -LiteralPath (Join-Path $root 'build')).Path
    $scratch = Get-Item -LiteralPath $work
    if ($scratch.Parent.FullName -ne $buildRoot -or $scratch.Name -ne "containment-$tag" -or
        ($scratch.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unexpected scratch path.' }
    Remove-Item -LiteralPath $scratch.FullName -Recurse -Force
    Write-Output 'Cleanup verified: package, certificate stores, signing key and staged files removed.'
}
