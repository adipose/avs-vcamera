<#
.SYNOPSIS
    Installs (or updates, or removes) the virtual camera on a target, over an
    existing PowerShell session.

.DESCRIPTION
    Copies build\out\x64 to C:\vcamera on the target, trusts the test-signing
    certificate there, and creates the root-enumerated device ROOT\VCamera
    with devcon -- or updates its driver if it exists.

    This is a kernel driver: the target must be in test-signing mode
    (bcdedit /set testsigning on, then reboot). The script checks and refuses
    rather than leaving a device that cannot start.

    Installing adds one DirectShow video capture device, "Virtual Camera
    (avs-vcamera)", with a video capture pin and an audio capture pin. It adds
    no audio endpoint: the audio pin is reachable only through the camera's
    own filter, as on a capture card.

.PARAMETER Session
    An open PSSession to the target. If you run a claim broker, claim first.

.PARAMETER Remove
    Remove the device and the driver package instead.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [System.Management.Automation.Runspaces.PSSession] $Session,
    [switch] $Remove
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$out = Join-Path $root 'build\out\x64'

if ($Remove) {
    Invoke-Command -Session $Session {
        $devcon = 'C:\vcamera\devcon.exe'
        if (Test-Path $devcon) { & $devcon remove 'ROOT\VCamera' | Out-Host }
        Get-WindowsDriver -Online | Where-Object { $_.OriginalFileName -match '\\vcamera\.inf' } |
            ForEach-Object { pnputil /delete-driver $_.Driver /uninstall /force | Out-Host }
    }
    return
}

foreach ($f in 'vcamera.sys', 'vcamera.inf', 'vcamera.cat', 'vcamera-test.cer', 'vcamprobe.exe', 'devcon.exe') {
    if (-not (Test-Path (Join-Path $out $f))) { throw "Missing $out\$f. Run tools\Build.ps1 first." }
}

$testSigning = Invoke-Command -Session $Session { [bool]((bcdedit /enum '{current}') -match 'testsigning\s+Yes') }
if (-not $testSigning) {
    throw 'The target is not in test-signing mode. Run "bcdedit /set testsigning on" there and reboot.'
}

Invoke-Command -Session $Session { New-Item -ItemType Directory -Force 'C:\vcamera' | Out-Null }
Copy-Item -ToSession $Session -Path (Join-Path $out '*') -Destination 'C:\vcamera\' -Force -Exclude '*.pdb'

Invoke-Command -Session $Session {
    $ErrorActionPreference = 'Stop'
    Set-Location 'C:\vcamera'

    # certutil rather than Import-Certificate: the cmdlet is refused with access denied in a remote session
    # on Windows 11, where certutil doing the same thing is not.
    foreach ($store in 'Root', 'TrustedPublisher') {
        certutil.exe -f -addstore $store 'C:\vcamera\vcamera-test.cer' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "certutil could not add the test certificate to $store (exit $LASTEXITCODE)" }
    }

    $existing = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object { $_.HardwareID -contains 'ROOT\VCamera' }
    if ($existing) {
        Write-Host 'Device exists; updating its driver ...'
        & .\devcon.exe update vcamera.inf 'ROOT\VCamera' | Out-Host
    } else {
        Write-Host 'Creating device ROOT\VCamera ...'
        & .\devcon.exe install vcamera.inf 'ROOT\VCamera' | Out-Host
    }
    if ($LASTEXITCODE -notin 0, 1) { throw "devcon failed with exit code $LASTEXITCODE" }   # 1 = reboot suggested

    Start-Sleep -Seconds 3
    Get-PnpDevice | Where-Object { $_.HardwareID -contains 'ROOT\VCamera' } |
        Format-Table FriendlyName, Status, Problem, InstanceId -AutoSize | Out-Host
}
