# Shared by Build.ps1 and Install-Toolchain.ps1: which kit versions this repository builds against, where the
# packages live, and where the tools inside them are. Dot-source it.

$script:KitPackages = @(
    @{ Id = 'Microsoft.Windows.SDK.CPP';     Version = '10.0.28000.2526'; Sha256 = 'BE1B419491607EAE6F7C57844EBAB39FACE9643C51E2AF1D9176A3BA0D0B23FC' }
    @{ Id = 'Microsoft.Windows.SDK.CPP.x64'; Version = '10.0.28000.2526'; Sha256 = 'A9CAE2A8C5DA7F5DC5838AE6A76D06D0D2E2FDC3D8CFC69CA6C184E4B9193A00' }
    @{ Id = 'Microsoft.Windows.WDK.x64';     Version = '10.0.28000.2526'; Sha256 = '63C939FB5A79295BF40E941DB592681272219B04EDFF095FE2F3D123E5579A90' }
)

function Get-KitToolchain {
    param([Parameter(Mandatory)] [string] $Root, [switch] $NoThrow)

    # The packages are 2.6 GB unpacked; a sibling repository that already fetched them can share its copy.
    $packages = if ($env:WDK_TOOLCHAIN_DIR) { $env:WDK_TOOLCHAIN_DIR } else { Join-Path $Root 'build\toolchain' }
    $dir = { param($id) $p = $script:KitPackages | Where-Object Id -eq $id; Join-Path $packages "$($p.Id).$($p.Version)\c" }

    $tc = [pscustomobject]@{
        PackagesDir = $packages
        Packages    = $script:KitPackages
        KitVersion  = '10.0.28000.0'
        SdkRoot     = & $dir 'Microsoft.Windows.SDK.CPP'
        SdkLibRoot  = & $dir 'Microsoft.Windows.SDK.CPP.x64'
        WdkRoot     = & $dir 'Microsoft.Windows.WDK.x64'
        Inf2Cat     = $null
        SignTool    = $null
        DevCon      = $null
    }
    $tc.Inf2Cat  = Join-Path $tc.WdkRoot "bin\$($tc.KitVersion)\x86\Inf2Cat.exe"
    $tc.SignTool = Join-Path $tc.SdkRoot "bin\$($tc.KitVersion)\x64\signtool.exe"
    $tc.DevCon   = Join-Path $tc.WdkRoot "tools\$($tc.KitVersion)\x64\devcon.exe"

    if (-not $NoThrow) {
        foreach ($f in @($tc.Inf2Cat, $tc.SignTool, $tc.DevCon, (Join-Path $tc.WdkRoot "Include\$($tc.KitVersion)\km\wdm.h"))) {
            if (-not (Test-Path $f)) { throw "Toolchain incomplete: $f is missing. Run tools\Install-Toolchain.ps1." }
        }
    }
    $tc
}
