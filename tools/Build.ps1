<#
.SYNOPSIS
    Builds the virtual camera driver (kernel mode, AVStream) and its test
    client, then catalogues and test-signs the driver package.

.DESCRIPTION
    The WDK and a matching SDK come from NuGet (Install-Toolchain.ps1) and the
    compiler from whatever Visual Studio is installed. The WDK's Visual Studio
    integration is not needed: this script applies by hand what its
    WindowsKernelModeDriver10.0 toolset would for a WDM driver -- /kernel, the
    km include and library paths, /DRIVER with GsDriverEntry -- and calls
    cl.exe and link.exe.

    Output goes to build\out\x64: vcamera.sys, vcamera.inf, vcamera.cat, the
    public half of the signing certificate, and vcamprobe.exe.

    A kernel driver only loads on a target in test-signing mode
    (bcdedit /set testsigning on) that trusts the certificate.

.PARAMETER CertName
    Subject CN of the test-signing certificate, created on first use.

.PARAMETER NoSign
    Build only.

.PARAMETER ToolsOnly
    Rebuild only the user-mode test client. The driver, its version stamp and
    its signature are left exactly as they are.
#>
[CmdletBinding()]
param(
    [string] $CertName = 'avs-vcamera test signing',
    [switch] $NoSign,
    [switch] $ToolsOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'Toolchain.ps1')
$tc = Get-KitToolchain -Root $root

$out = Join-Path $root 'build\out\x64'
$obj = Join-Path $root 'build\obj\x64'
New-Item -ItemType Directory -Force $out, $obj | Out-Null

# --- Compiler environment ---------------------------------------------------

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path $vswhere)) { throw 'vswhere.exe not found; install Visual Studio with the C++ workload.' }
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'No Visual Studio with the C++ tools found.' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$envDump = & cmd /c "`"$vcvars`" >nul 2>&1 && set"
$vcEnv = @{}
foreach ($line in $envDump) {
    $i = $line.IndexOf('=')
    if ($i -gt 0) { $vcEnv[$line.Substring(0, $i)] = $line.Substring($i + 1) }
}
$vcToolsDir = $vcEnv['VCToolsInstallDir']
if (-not $vcToolsDir) { throw "vcvars64.bat did not set VCToolsInstallDir ($vcvars)." }
$env:PATH = $vcEnv['PATH']

# Stamp a version that is unique to this build into the staged INF. PnP compares DriverVer before anything else:
# a rebuilt package with the same version is "already installed", and the old binary stays in the driver store
# while devcon reports success.
function Set-InfDriverVer {
    param([string] $Path)
    $now = Get-Date
    $ver = '1.{0}.{1}.{2}' -f (($now.Year - 2000) * 1000 + $now.DayOfYear), ($now.Hour * 100 + $now.Minute), $now.Second
    $stamp = 'DriverVer={0},{1}' -f $now.ToString('MM/dd/yyyy', [Globalization.CultureInfo]::InvariantCulture), $ver
    $text = [IO.File]::ReadAllText($Path)
    $new = [regex]::Replace($text, '(?m)^DriverVer\s*=.*$', $stamp)
    if ($new -eq $text) { throw "No DriverVer line found in $Path" }
    [IO.File]::WriteAllText($Path, $new)
    Write-Host "Driver version $ver" -ForegroundColor Cyan
}

function Invoke-Tool {
    param([string] $Exe, [string[]] $Arguments, [string] $What)
    & $Exe @Arguments 2>&1 | ForEach-Object { Write-Host $_ }
    if ($LASTEXITCODE -ne 0) { throw "$What failed with exit code $LASTEXITCODE" }
}

$ver = $tc.KitVersion
$wdkInc = Join-Path $tc.WdkRoot "Include\$ver"
$sdkInc = Join-Path $tc.SdkRoot "Include\$ver"
$src = Join-Path $root 'src'
$driverSrc = Join-Path $src 'driver'

if (-not $ToolsOnly) {

# Kernel code sees the WDK's km headers and its own CRT subset, never the user-mode CRT. ks.h and ksmedia.h
# live in the SDK's shared directory.
$env:INCLUDE = @(
    $driverSrc,
    (Join-Path $wdkInc 'km'), (Join-Path $wdkInc 'km\crt'), (Join-Path $wdkInc 'shared'), (Join-Path $sdkInc 'shared')
) -join ';'
$env:LIB = (Join-Path $tc.WdkRoot "Lib\$ver\km\x64")

$sources = Get-ChildItem $driverSrc -Include *.cpp, *.c -File -Recurse | ForEach-Object FullName

Write-Host "Compiling vcamera.sys ($($sources.Count) files) ..." -ForegroundColor Cyan

$clArgs = @(
    '/nologo', '/c', '/kernel', '/W3', '/WX', '/wd4996', '/O2', '/Oi', '/Zi', '/GF', '/Gy', '/Zp8', '/GS', '/GR-',
    '/D_WIN64', '/D_AMD64_', '/DAMD64', '/D_WIN32', '/DUNICODE', '/D_UNICODE', '/DWINNT=1', '/DSTD_CALL',
    '/D_WIN32_WINNT=0x0A00', '/DWINVER=0x0A00', '/DNTDDI_VERSION=0x0A000008',   # Windows 10 2004, as vaudio-endpoint
    '/DDEPRECATE_DDK_FUNCTIONS=1', '/DPOOL_NX_OPTIN=1', '/DNDEBUG',
    '/D_WIN2K_COMPAT_SLIST_USAGE', '/D_NO_SYS_GUID_OPERATOR_EQ_', '/DDEBUG_LEVEL=DEBUGLVL_TERSE',
    "/Fo$obj/", "/Fd$obj/vcamera.pdb"
) + $sources
Invoke-Tool cl.exe $clArgs 'cl'

$objects = $sources | ForEach-Object { Join-Path $obj ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj') }

$linkArgs = @(
    '/nologo', '/DRIVER', '/SUBSYSTEM:NATIVE,10.00', '/ENTRY:GsDriverEntry', '/NODEFAULTLIB',
    '/DEBUG', '/OPT:REF', '/OPT:ICF', '/RELEASE', '/MERGE:_TEXT=.text;_PAGE=PAGE', '/SECTION:INIT,d',
    '/INTEGRITYCHECK', '/NXCOMPAT', '/DYNAMICBASE',
    "/OUT:$out\vcamera.sys", "/PDB:$out\vcamera.pdb",
    'BufferOverflowFastFailK.lib', 'ntoskrnl.lib', 'hal.lib', 'wmilib.lib',
    'ks.lib', 'ksguid.lib', 'libcntpr.lib', 'ntstrsafe.lib'
) + $objects
Invoke-Tool link.exe $linkArgs 'link'

Copy-Item (Join-Path $driverSrc 'vcamera.inf') $out -Force
Set-InfDriverVer (Join-Path $out 'vcamera.inf')

}   # -not $ToolsOnly

# --- Test client -------------------------------------------------------------
#
# vcamprobe is an ordinary user-mode DirectShow program: the VC runtime and the SDK's um headers.

Write-Host 'Compiling vcamprobe.exe ...' -ForegroundColor Cyan

$env:INCLUDE = @(
    (Join-Path $vcToolsDir 'include'),
    (Join-Path $sdkInc 'ucrt'), (Join-Path $sdkInc 'shared'), (Join-Path $sdkInc 'um')
) -join ';'
$env:LIB = @(
    (Join-Path $vcToolsDir 'lib\x64'),
    (Join-Path $tc.SdkLibRoot 'ucrt\x64'), (Join-Path $tc.SdkLibRoot 'um\x64')
) -join ';'

$clArgs = @(
    '/nologo', '/W4', '/WX', '/O2', '/EHsc', '/MT', '/std:c++17', '/permissive-',
    '/DUNICODE', '/D_UNICODE', '/D_WIN32_WINNT=0x0A00',
    "/Fo$obj/", "/Fe$out/vcamprobe.exe",
    (Join-Path $src 'tools\vcamprobe.cpp'),
    '/link', '/SUBSYSTEM:CONSOLE', 'ole32.lib', 'oleaut32.lib', 'strmiids.lib', 'uuid.lib'
)
Invoke-Tool cl.exe $clArgs 'cl (vcamprobe)'

if ($ToolsOnly) {
    Write-Host "Built the test client only: $out" -ForegroundColor Green
    return
}

if ($NoSign) {
    Write-Host "Built (unsigned): $out" -ForegroundColor Green
    return
}

# --- Catalogue and sign -----------------------------------------------------

$cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq "CN=$CertName" -and $_.NotAfter -gt (Get-Date) } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
if (-not $cert) {
    Write-Host "Creating test certificate CN=$CertName ..." -ForegroundColor Cyan
    $cert = New-SelfSignedCertificate -Subject "CN=$CertName" -Type CodeSigningCert -KeyUsage DigitalSignature `
        -CertStoreLocation Cert:\CurrentUser\My -NotAfter (Get-Date).AddYears(5) -KeyExportPolicy Exportable
}
Export-Certificate -Cert $cert -FilePath (Join-Path $out 'vcamera-test.cer') -Force | Out-Null

$pkg = Join-Path $root 'build\package'
New-Item -ItemType Directory -Force $pkg | Out-Null
Get-ChildItem $pkg | ForEach-Object { [IO.File]::Delete($_.FullName) }
Copy-Item (Join-Path $out 'vcamera.sys'), (Join-Path $out 'vcamera.inf') $pkg

Write-Host 'Building catalogue ...' -ForegroundColor Cyan
Invoke-Tool $tc.Inf2Cat @("/driver:$pkg", '/os:10_X64', '/uselocaltime') 'inf2cat'

foreach ($f in @('vcamera.cat', 'vcamera.sys')) {
    Invoke-Tool $tc.SignTool @('sign', '/fd', 'SHA256', '/sha1', $cert.Thumbprint, '/s', 'My', (Join-Path $pkg $f)) "signtool ($f)"
}
Copy-Item (Join-Path $pkg '*') $out -Force
Copy-Item $tc.DevCon $out -Force

Write-Host "Built and signed: $out" -ForegroundColor Green
