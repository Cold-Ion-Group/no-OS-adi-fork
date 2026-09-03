<#
.SYNOPSIS
Build all six FMCDAC firmware profiles from one XSA.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$XsaPath,

    [string]$XilinxSettings = 'C:\Xilinx\Vitis\2021.2\settings64.bat',

    [Parameter(Mandatory = $true)]
    [string]$ArtifactRoot,

    [ValidateRange(1, 64)]
    [int]$Jobs = 4,

    [switch]$AllowDirtySource,
    [string]$MakeCommand = 'make'
)

$ErrorActionPreference = 'Stop'
$builder = Join-Path $PSScriptRoot 'build_phase_f_firmware.ps1'
if (-not (Test-Path -LiteralPath $builder -PathType Leaf)) {
    throw "Firmware build wrapper not found: $builder"
}

$profiles = @(
    'default',
    'scheduler-preload',
    'scheduler-stream',
    'scheduler-dma',
    'scheduler-eth',
    'scheduler-eth-release'
)

foreach ($profile in $profiles) {
    Write-Host "Building FMCDAC profile: $profile"
    & $builder `
        -XsaPath $XsaPath `
        -Profile $profile `
        -XilinxSettings $XilinxSettings `
        -ArtifactRoot $ArtifactRoot `
        -Jobs $Jobs `
        -AllowDirtySource:$AllowDirtySource `
        -MakeCommand $MakeCommand
    if (-not $?) {
        throw "FMCDAC profile build failed: $profile"
    }
}

Write-Host 'All FMCDAC firmware profiles completed.'
