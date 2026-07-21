<#
.SYNOPSIS
Program a KCU116 with a matching XSA and FMCDAC ELF.

.DESCRIPTION
The XSA must include its bitstream. This script extracts that bitstream to a
temporary directory, programs the FPGA, downloads the MicroBlaze ELF, and
starts execution through the existing no-OS XSCT utility.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$XsaPath,

    [Parameter(Mandatory = $true)]
    [string]$ElfPath,

    [Parameter(Mandatory = $true)]
    [string]$ManifestPath,

    [string]$XilinxSettings = 'C:\Xilinx\Vitis\2021.2\settings64.bat',
    [string]$JtagCableId = '',

    [Parameter(Mandatory = $true)]
    [string]$LogPath
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

function Assert-ManifestHash {
    param(
        [string]$Path,
        [string]$ExpectedHash,
        [string]$Label
    )

    if ([string]::IsNullOrWhiteSpace($ExpectedHash)) {
        throw "Manifest does not contain a $Label SHA-256 hash."
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if (-not $actual.Equals($ExpectedHash,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label does not match the selected manifest: $Path"
    }
}

function Extract-XsaBitstream {
    param([string]$Xsa, [string]$Destination)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($Xsa)
    try {
        $bitEntries = @(
            $archive.Entries | Where-Object { $_.Name -like '*.bit' }
        )
        if ($bitEntries.Count -eq 0) {
            throw 'The XSA does not contain a bitstream. Export the XSA with the bitstream included.'
        }
        $entry = @(
            $bitEntries | Where-Object { $_.Name -ieq 'system_top.bit' }
        ) | Select-Object -First 1
        if (-not $entry) {
            if ($bitEntries.Count -ne 1) {
                throw 'The XSA contains multiple bitstreams and none is named system_top.bit.'
            }
            $entry = $bitEntries[0]
        }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $Destination, $true)
    } finally {
        $archive.Dispose()
    }
}

$xsa = Resolve-RequiredFile $XsaPath 'XSA'
$elf = Resolve-RequiredFile $ElfPath 'ELF'
$manifestFile = Resolve-RequiredFile $ManifestPath 'Build manifest'
$manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json
$manifestXsaHash = if ($manifest.hdl.xsa) {
    [string]$manifest.hdl.xsa.sha256
} else {
    [string]$manifest.hdl.sha256
}
$manifestElfHash = [string]$manifest.firmware.elf.sha256
Assert-ManifestHash $xsa $manifestXsaHash 'XSA'
Assert-ManifestHash $elf $manifestElfHash 'ELF'
$projectDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $projectDir '..\..')).Path
$utilTcl = Join-Path $repoRoot 'tools\scripts\platform\xilinx\util.tcl'
$utilTcl = Resolve-RequiredFile $utilTcl 'no-OS Xilinx utility'

Import-BatchEnvironment $XilinxSettings
if (-not (Get-Command xsct -ErrorAction SilentlyContinue)) {
    throw 'xsct is not on PATH after loading the Xilinx environment.'
}

$resolvedLog = [IO.Path]::GetFullPath($LogPath)
$logDir = Split-Path -Parent $resolvedLog
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$temporary = Join-Path ([IO.Path]::GetTempPath()) ("fmcdac_program_" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $temporary | Out-Null

try {
    $stagedXsa = Join-Path $temporary 'system_top.xsa'
    $stagedBit = Join-Path $temporary 'system_top.bit'
    Copy-Item -LiteralPath $xsa -Destination $stagedXsa
    Extract-XsaBitstream $xsa $stagedBit

    $arguments = @(
        $utilTcl,
        'upload',
        $temporary,
        $temporary,
        'system_top.xsa',
        $elf,
        '0',
        'Empty Application(C)',
        $JtagCableId
    )

    "> xsct $($arguments -join ' ')" | Tee-Object -FilePath $resolvedLog
    & xsct @arguments 2>&1 | Tee-Object -FilePath $resolvedLog -Append
    if ($LASTEXITCODE -ne 0) {
        throw "XSCT programming failed with exit code $LASTEXITCODE. See $resolvedLog"
    }
} finally {
    if (Test-Path -LiteralPath $temporary) {
        Remove-Item -LiteralPath $temporary -Recurse -Force
    }
}

Write-Host "Programming complete. Log: $resolvedLog"
Write-Host "Verified manifest: $manifestFile"
