<#
.SYNOPSIS
Build one FMCDAC firmware profile from an explicit XSA.

.DESCRIPTION
Run this on the Windows build machine from a checkout whose path has no spaces.
The script imports the Vitis environment, recreates the no-OS build directory,
builds the selected profile, and copies the important files to an external
artifact directory. Use -Program only when the KCU116 is connected by JTAG.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$XsaPath,

    [ValidateSet('default', 'scheduler-preload', 'scheduler-stream',
                 'scheduler-dma', 'scheduler-eth', 'scheduler-eth-release')]
    [string]$Profile = 'scheduler-eth',

    [string]$XilinxSettings = 'C:\Xilinx\Vitis\2021.2\settings64.bat',

    [Parameter(Mandatory = $true)]
    [string]$ArtifactRoot,

    [ValidateRange(1, 64)]
    [int]$Jobs = 4,

    [switch]$Program,
    [switch]$AllowDirtySource,
    [string]$JtagCableId = '',
    [string]$MakeCommand = 'make'
)

$ErrorActionPreference = 'Stop'

function Resolve-RequiredFile {
    param([string]$Path, [string]$Label)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Import-BatchEnvironment {
    param([string]$BatchFile)

    $resolved = Resolve-RequiredFile $BatchFile 'Xilinx settings batch file'
    $command = 'call "{0}" >nul && set' -f $resolved
    $lines = & $env:ComSpec /d /s /c $command
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to import Xilinx environment from $resolved"
    }
    foreach ($line in $lines) {
        if ($line -match '^([^=]+)=(.*)$' -and -not $Matches[1].StartsWith('=')) {
            [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
        }
    }
}

function Invoke-LoggedCommand {
    param(
        [string]$FilePath,
        [string[]]$Arguments,
        [string]$LogPath
    )

    "`n> $FilePath $($Arguments -join ' ')" | Tee-Object -FilePath $LogPath -Append
    & $FilePath @Arguments 2>&1 | Tee-Object -FilePath $LogPath -Append
    if ($LASTEXITCODE -ne 0) {
        throw "$FilePath failed with exit code $LASTEXITCODE. See $LogPath"
    }
}

function Copy-RequiredArtifact {
    param([string]$Source, [string]$DestinationDirectory, [string]$Label)

    if (-not (Test-Path -LiteralPath $Source -PathType Leaf)) {
        throw "$Label was not produced: $Source"
    }
    Copy-Item -LiteralPath $Source -Destination $DestinationDirectory -Force
}

$projectDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectDir '..\..')).Path
$xsa = Resolve-RequiredFile $XsaPath 'XSA'
$localBuildEnv = Join-Path $projectDir 'fmcdac_build.env'

if (Test-Path -LiteralPath $localBuildEnv -PathType Leaf) {
    throw 'The clean wrapper does not use fmcdac_build.env. Move it aside or use the manual build procedure.'
}
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw 'Git is required to record and validate the source revision.'
}
$dirtySource = @(& git -C $repoRoot status --porcelain)
if ($LASTEXITCODE -ne 0) {
    throw "Could not read Git status for $repoRoot"
}
if ($dirtySource.Count -gt 0 -and -not $AllowDirtySource) {
    throw 'The no-OS worktree is dirty. Commit/stash it, or pass -AllowDirtySource for a development build.'
}

if ($repoRoot -match '\s' -or $xsa -match '\s') {
    throw 'The no-OS checkout and XSA path must not contain spaces.'
}
if (-not [IO.Path]::IsPathRooted($ArtifactRoot)) {
    throw '-ArtifactRoot must be an absolute path outside the Git worktree.'
}
$resolvedArtifactRoot = [IO.Path]::GetFullPath($ArtifactRoot)
$repoPrefix = $repoRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if ($resolvedArtifactRoot.Equals($repoRoot,
        [StringComparison]::OrdinalIgnoreCase) -or
    $resolvedArtifactRoot.StartsWith($repoPrefix,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw '-ArtifactRoot must be outside the no-OS Git worktree.'
}
if (-not (Test-Path -LiteralPath $resolvedArtifactRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $resolvedArtifactRoot -Force | Out-Null
}

Import-BatchEnvironment $XilinxSettings

foreach ($tool in @($MakeCommand, 'xsct', 'mb-gcc')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        throw "Required build tool is not on PATH after loading Vitis: $tool"
    }
}

$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')
$artifactDir = Join-Path $resolvedArtifactRoot "${stamp}_${Profile}"
$firmwareDir = Join-Path $artifactDir 'firmware'
$hardwareDir = Join-Path $artifactDir 'hardware'
if (Test-Path -LiteralPath $artifactDir) {
    throw "Artifact directory already exists: $artifactDir"
}
New-Item -ItemType Directory -Path $artifactDir | Out-Null
New-Item -ItemType Directory -Path $firmwareDir, $hardwareDir | Out-Null

$buildLog = Join-Path $firmwareDir 'build.log'
$makeXsa = $xsa.Replace('\', '/')
$common = @(
    '-C', $projectDir,
    "FMCDAC_XSA=$makeXsa",
    "FMCDAC_AWG_PROFILE=$Profile",
    'SKIP_MANIFEST=1'
)

Invoke-LoggedCommand $MakeCommand ($common + @('fmcdac-build-config')) $buildLog
Invoke-LoggedCommand $MakeCommand ($common + @('reset')) $buildLog
Invoke-LoggedCommand $MakeCommand ($common + @("-j$Jobs")) $buildLog

$buildDir = Join-Path $projectDir 'build'
$elf = Join-Path $buildDir 'fmcdac.elf'
Copy-RequiredArtifact $elf $firmwareDir 'Firmware ELF'
Copy-RequiredArtifact $xsa $hardwareDir 'XSA'

$xparameters = Get-ChildItem -LiteralPath (Join-Path $buildDir 'bsp') `
    -Recurse -Filter xparameters.h -File | Select-Object -First 1
if (-not $xparameters) {
    throw "Generated xparameters.h was not found below $buildDir\bsp"
}
Copy-Item -LiteralPath $xparameters.FullName -Destination $firmwareDir -Force

$linkerScript = Join-Path $buildDir 'app\src\lscript.ld'
Copy-RequiredArtifact $linkerScript $firmwareDir 'Generated linker script'

$manifestScript = Join-Path $projectDir 'gen_manifest.ps1'
$manifestPath = Join-Path $artifactDir 'manifest.json'
& $manifestScript `
    -XsaPath $xsa `
    -ElfPath $elf `
    -XparametersPath $xparameters.FullName `
    -LinkerScriptPath $linkerScript `
    -BuildLogPath $buildLog `
    -Profile $Profile `
    -OutputPath $manifestPath
if (-not $?) {
    throw 'Manifest generation failed.'
}

if ($Program) {
    $programScript = Join-Path $projectDir 'program_phase_f_firmware.ps1'
    $programLog = Join-Path $firmwareDir 'program.log'
    & $programScript `
        -XsaPath $xsa `
        -ElfPath $elf `
        -ManifestPath $manifestPath `
        -XilinxSettings $XilinxSettings `
        -JtagCableId $JtagCableId `
        -LogPath $programLog
    if (-not $?) {
        throw 'Programming failed.'
    }
}

Write-Host "FMCDAC firmware build complete."
Write-Host "Profile : $Profile"
Write-Host "ELF     : $elf"
Write-Host "Artifacts: $artifactDir"
