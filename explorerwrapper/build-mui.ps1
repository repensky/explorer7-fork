<#
.SYNOPSIS
    Builds wrp64.dll and splits it into a language neutral DLL plus an en-US MUI.

.PARAMETER Name
    Output folder name under Release. Defaults to release-final.

.PARAMETER Language
    MUI language folder and muirct LCID. Defaults to en-US / 0409.

.PARAMETER Configuration
    MSBuild configuration. Defaults to Release.

.PARAMETER SkipBuild
    Re-split the existing Release\wrp64.dll without rebuilding.

.EXAMPLE
    .\build-mui.ps1
    .\build-mui.ps1 -Name test7
    .\build-mui.ps1 -Name fr -Language fr-FR -Lcid 040C
#>
[CmdletBinding()]
param(
    [string]$Name = 'release-final',
    [string]$Language = 'en-US',
    [string]$Lcid = '0409',
    [ValidateSet('Release', 'Debug')]
    [string]$Configuration = 'Release',
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

$root     = $PSScriptRoot
$project  = Join-Path $root 'explorerwrapper.vcxproj'
$rcconfig = Join-Path $root 'wrp64.rcconfig'
$outRoot  = Join-Path $root $Configuration
$builtDll = Join-Path $outRoot 'wrp64.dll'
$destDir  = Join-Path $outRoot $Name
$muirct   = 'C:\Windows\Classic\SigningTool\x64\muirct.exe'

foreach ($p in @($project, $rcconfig)) {
    if (-not (Test-Path $p)) { throw "missing required file: $p" }
}
if (-not (Test-Path $muirct)) { throw "muirct not found at $muirct" }

# ---- Build ------------------------------------------------
if (-not $SkipBuild) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { throw "vswhere not found at $vswhere" }

    $msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
                          -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
    if (-not $msbuild) { throw 'MSBuild not found, install the v143 toolset' }

    Write-Host "MSBuild : $msbuild"
    Write-Host "Building: $Configuration x64"

    & $msbuild $project /p:Configuration=$Configuration /p:Platform=x64 /t:Rebuild /v:minimal /nologo /m:1
    if ($LASTEXITCODE -ne 0) { throw "build failed with exit code $LASTEXITCODE" }
}

if (-not (Test-Path $builtDll)) { throw "no DLL at $builtDll" }

# ---- MUI split --------------------------------------------
# Neutral resources stay in the DLL, strings move to the .mui, per wrp64.rcconfig
if (Test-Path $destDir) { Remove-Item $destDir -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $destDir $Language) | Out-Null

$lnOut  = Join-Path $destDir 'wrp64.dll'
$muiOut = Join-Path $destDir "$Language\wrp64.dll.mui"

& $muirct -q $rcconfig -x $Lcid -g $Lcid $builtDll $lnOut $muiOut
if ($LASTEXITCODE -ne 0) { throw "muirct failed with exit code $LASTEXITCODE" }

# ---- Report -----------------------------------------------
$unsplit = (Get-Item $builtDll).Length
$neutral = (Get-Item $lnOut).Length
$mui     = (Get-Item $muiOut).Length

Write-Host ''
Write-Host "OK  $destDir"
Write-Host ("     unsplit  {0,9:N0} bytes" -f $unsplit)
Write-Host ("     neutral  {0,9:N0} bytes  wrp64.dll" -f $neutral)
Write-Host ("     mui      {0,9:N0} bytes  $Language\wrp64.dll.mui" -f $mui)

if ($mui -le 1024) {
    Write-Warning 'MUI looks empty, check that the RC still has a STRINGTABLE'
}
if ($neutral -ge $unsplit) {
    Write-Warning 'Neutral DLL did not shrink, resources may not have split'
}
