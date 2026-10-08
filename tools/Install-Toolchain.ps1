<#
.SYNOPSIS
    Downloads and verifies the Windows SDK and WDK NuGet packages the driver
    builds against, into build\toolchain (gitignored, about 2.6 GB unpacked).

.DESCRIPTION
    Since 10.0.26100 Microsoft publishes the WDK as NuGet packages, so a build
    needs neither the WDK installer nor its Visual Studio extension. A .nupkg
    is a zip: each is fetched from nuget.org's flat container, checked against
    a pinned SHA256, and unpacked. Idempotent -- a package already unpacked
    from a verified archive is skipped.

.PARAMETER Force
    Re-download and re-extract even when present packages verify.
#>
[CmdletBinding()]
param([switch] $Force)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'Toolchain.ps1')
$tc = Get-KitToolchain -Root $root -NoThrow
New-Item -ItemType Directory -Force $tc.PackagesDir | Out-Null

foreach ($p in $tc.Packages) {
    $name = "$($p.Id).$($p.Version)"
    $dir = Join-Path $tc.PackagesDir $name
    $nupkg = Join-Path $dir "$name.nupkg"
    $marker = Join-Path $dir '.extracted'

    $verified = (Test-Path $nupkg) -and ((Get-FileHash $nupkg -Algorithm SHA256).Hash -eq $p.Sha256)
    if ($verified -and (Test-Path $marker) -and -not $Force) {
        Write-Host "$name present and verified." -ForegroundColor Green
        continue
    }

    New-Item -ItemType Directory -Force $dir | Out-Null
    if (-not $verified -or $Force) {
        $id = $p.Id.ToLowerInvariant()
        $url = "https://api.nuget.org/v3-flatcontainer/$id/$($p.Version)/$id.$($p.Version).nupkg"
        Write-Host "Downloading $name ..." -ForegroundColor Cyan
        Invoke-WebRequest -Uri $url -OutFile "$nupkg.partial" -UseBasicParsing
        $hash = (Get-FileHash "$nupkg.partial" -Algorithm SHA256).Hash
        if ($hash -ne $p.Sha256) {
            Remove-Item "$nupkg.partial" -Force
            throw "$name: SHA256 $hash does not match the pinned $($p.Sha256)."
        }
        Move-Item "$nupkg.partial" $nupkg -Force
    }

    Write-Host "Extracting $name ..." -ForegroundColor Cyan
    $zip = Join-Path $dir "$name.zip"
    Copy-Item $nupkg $zip -Force
    try { Expand-Archive -Path $zip -DestinationPath $dir -Force } finally { Remove-Item $zip -Force }
    Set-Content $marker (Get-Date -Format o)
}

$null = Get-KitToolchain -Root $root
Write-Host "Toolchain ready: $($tc.PackagesDir)" -ForegroundColor Green
