<#
.SYNOPSIS
    Records the virtual camera on a target in every format it offers, with its
    audio, and checks each recording: frame numbers advancing with no repeats,
    and the camera's tone on each audio channel.

.DESCRIPTION
    Runs vcamprobe on the target (installed in C:\vcamera by
    tools\Install-VCamera.ps1) to record each capability to an uncompressed
    AVI through DirectShow's AVI mux, copies the files back and judges them
    with tests\avicheck.py. Needs Python 3 on this machine, nothing on the
    target beyond the driver.

    Results go to build\test-results\<timestamp>: the AVIs and summary.json.

.PARAMETER Session
    An open PSSession to the target.

.PARAMETER Seconds
    Length of each recording.

.PARAMETER MaxGap
    The most consecutive frames a recording may lose. A guest rendering with
    WARP drops the odd frame under load; a frozen picture is caught by the
    repeat check whatever this is.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [System.Management.Automation.Runspaces.PSSession] $Session,
    [int] $Seconds = 6,
    [int] $MaxGap = 3
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$out = Join-Path $root ("build\test-results\" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
New-Item -ItemType Directory -Force $out | Out-Null

$probe = Join-Path $root 'build\out\x64\vcamprobe.exe'
if (Test-Path $probe) { Copy-Item -ToSession $Session $probe 'C:\vcamera\' -Force }

$list = Invoke-Command -Session $Session { & C:\vcamera\vcamprobe.exe list } | Out-String | ConvertFrom-Json
$camera = $list.devices | Where-Object friendlyName -like '*avs-vcamera*' | Select-Object -First 1
if (-not $camera) { throw 'The target lists no avs-vcamera device. Install it with tools\Install-VCamera.ps1.' }
$camera | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $out 'device.json')

$minSeconds = $Seconds - 1
$results = foreach ($cap in $camera.video) {
    $name = '{0}-{1}x{2}' -f $cap.subtype, $cap.width, [Math]::Abs($cap.height)
    $size = '{0}x{1}' -f $cap.width, [Math]::Abs($cap.height)
    $guestFile = "C:\vcamera\rec\$name.avi"
    $rec = Invoke-Command -Session $Session -ArgumentList $guestFile, $Seconds, $size, $cap.subtype {
        param($file, $seconds, $size, $format)
        New-Item -ItemType Directory -Force (Split-Path $file) | Out-Null
        & C:\vcamera\vcamprobe.exe record $file --seconds $seconds --size $size --format $format --device avs-vcamera 2>&1 | Out-String
    }
    $local = Join-Path $out "$name.avi"
    Copy-Item -FromSession $Session $guestFile $local -Force
    Invoke-Command -Session $Session -ArgumentList $guestFile { param($f) [IO.File]::Delete($f) }
    $check = & py (Join-Path $PSScriptRoot 'avicheck.py') $local --min-seconds $minSeconds --expect-tones 1000,1500 --max-gap $MaxGap | Out-String | ConvertFrom-Json
    $v = $check.streams | Where-Object type -eq 'vids' | Select-Object -First 1
    $a = $check.streams | Where-Object type -eq 'auds' | Select-Object -First 1
    $r = [pscustomobject]@{
        Format = $name; Pass = ($check.problems.Count -eq 0)
        Frames = if ($v) { $v.frames.read } else { 0 }
        First = if ($v) { $v.frames.first } else { $null }; Last = if ($v) { $v.frames.last } else { $null }
        Repeats = if ($v) { $v.frames.repeats } else { $null }; Missing = if ($v) { $v.frames.missing } else { $null }
        AudioSeconds = if ($a) { $a.seconds } else { 0 }; Tones = if ($a) { $a.frequencies -join '/' } else { '' }
        Problems = $check.problems -join '; '; Probe = $rec.Trim()
    }
    $r | Format-Table Format, Pass, Frames, First, Last, Repeats, Missing, AudioSeconds, Tones -AutoSize | Out-Host
    $r
}

$results | ConvertTo-Json -Depth 3 | Set-Content (Join-Path $out 'summary.json')
$failed = @($results | Where-Object { -not $_.Pass })
foreach ($f in $failed) { Write-Host "FAIL $($f.Format): $($f.Problems)" -ForegroundColor Red }
Write-Host ("{0} of {1} formats passed; results in {2}" -f ($results.Count - $failed.Count), $results.Count, $out) -ForegroundColor $(if ($failed) { 'Red' } else { 'Green' })
