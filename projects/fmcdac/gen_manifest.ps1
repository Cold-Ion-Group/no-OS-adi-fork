<#
.SYNOPSIS
    Generate manifest.json for FMCDAC build reproducibility.

.DESCRIPTION
    Captures exact git commit SHAs, submodule states, selected profile,
    XSA/bitstream/ELF hashes, and JESD build configuration in one JSON file.
    Run from the projects/fmcdac/ directory.

.EXAMPLE
    .\gen_manifest.ps1
    .\gen_manifest.ps1 -Verify
    .\gen_manifest.ps1 -XsaPath D:\build\system_top.xsa `
        -ElfPath .\build\fmcdac.elf -Profile scheduler-eth `
        -OutputPath D:\artifacts\manifest.json
#>
param(
    [switch]$Verify,
    [string]$XsaPath = (Join-Path $PSScriptRoot 'system_top.xsa'),
    [string]$BitPath = '',
    [string]$ElfPath = '',
    [string]$XparametersPath = '',
    [string]$LinkerScriptPath = '',
    [string]$BuildLogPath = '',
    [string]$BuildConfigPath = '',
    [ValidateSet('unknown', 'default', 'scheduler-preload', 'scheduler-stream',
                 'scheduler-dma', 'scheduler-eth')]
    [string]$Profile = 'unknown',
    [string]$OutputPath = (Join-Path $PSScriptRoot 'manifest.json')
)

$ErrorActionPreference = 'Stop'
$ProjectDir = $PSScriptRoot
$RepoRoot   = (git -C $ProjectDir rev-parse --show-toplevel 2>$null)
if (-not $RepoRoot) {
    Write-Error 'Not inside a git repository. Run from projects/fmcdac/.'
    exit 1
}

# --- Gather git state ---
$fwCommit    = (git -C $RepoRoot rev-parse HEAD).Trim()
$fwBranch    = (git -C $RepoRoot rev-parse --abbrev-ref HEAD).Trim()
$fwDescribe  = (git -C $RepoRoot describe --all --long --dirty=-modified).Trim()
$fwDirty     = $null -ne (git -C $RepoRoot status --porcelain)

# --- Gather submodule state ---
$submodules = @{}
$subRaw = git -C $RepoRoot submodule status --recursive 2>$null
if ($subRaw) {
    foreach ($line in $subRaw) {
        if ($line -match '^\s*[+\-U]?([0-9a-f]+)\s+(\S+)') {
            $submodules[$Matches[2]] = $Matches[1]
        }
    }
}

function Get-ArtifactRecord {
    param([string]$Path, [string]$Label)

    if (-not $Path) {
        return [ordered]@{ file = ''; sha256 = '' }
    }
    $fullPath = [IO.Path]::GetFullPath($Path)
    if (-not (Test-Path -LiteralPath $fullPath -PathType Leaf)) {
        Write-Warning "$Label not found - hash will be empty: $fullPath"
        return [ordered]@{ file = $fullPath; sha256 = '' }
    }
    return [ordered]@{
        file = $fullPath
        sha256 = (Get-FileHash -Algorithm SHA256 -LiteralPath $fullPath).Hash.ToLower()
    }
}

# --- Hash build artifacts ---
$xsaArtifact = Get-ArtifactRecord $XsaPath 'XSA'
$bitArtifact = Get-ArtifactRecord $BitPath 'Bitstream'
$elfArtifact = Get-ArtifactRecord $ElfPath 'ELF'
$xparametersArtifact = Get-ArtifactRecord $XparametersPath 'xparameters.h'
$linkerArtifact = Get-ArtifactRecord $LinkerScriptPath 'linker script'
$buildLogArtifact = Get-ArtifactRecord $BuildLogPath 'build log'
$buildConfigArtifact = Get-ArtifactRecord $BuildConfigPath 'build configuration file'

if (-not $xsaArtifact.sha256) {
    throw "Cannot record a missing XSA: $($xsaArtifact.file)"
}
foreach ($requested in @(
    @{ Path = $BitPath; Record = $bitArtifact; Label = 'bitstream' },
    @{ Path = $ElfPath; Record = $elfArtifact; Label = 'ELF' },
    @{ Path = $XparametersPath; Record = $xparametersArtifact; Label = 'xparameters.h' },
    @{ Path = $LinkerScriptPath; Record = $linkerArtifact; Label = 'linker script' },
    @{ Path = $BuildLogPath; Record = $buildLogArtifact; Label = 'build log' },
    @{ Path = $BuildConfigPath; Record = $buildConfigArtifact; Label = 'build configuration file' }
)) {
    if ($requested.Path -and -not $requested.Record.sha256) {
        throw "Cannot record a missing $($requested.Label): $($requested.Record.file)"
    }
}

# --- Build config constants (must match fmcdac.c source) ---
$buildConfig = [ordered]@{
    jesd_subclass         = 1
    jesd_mode             = 4
    lane_rate_kbps        = 9830400
    dac_freq_khz          = 1966080
    ref_freq_khz          = 122880
    interpolation         = 2
    scrambling            = 1
    num_converters        = 2
    num_lanes             = 4
    octets_per_frame      = 1
    frames_per_multiframe = 32
    bits_per_sample       = 16
    high_density          = 1
    lane_mux              = @(4, 5, 6, 7, 0, 1, 2, 3)
    lane_polarity_invert  = '0x04'
}

# --- Assemble manifest ---
$manifest = [ordered]@{
    project   = 'fmcdac_kcu116'
    created   = [DateTime]::UtcNow.ToString('o')
    firmware  = [ordered]@{
        repo     = 'Cold-Ion-Group/no-OS-adi-fork'
        branch   = $fwBranch
        commit   = $fwCommit
        describe = $fwDescribe
        dirty    = $fwDirty
        profile  = $Profile
        elf      = $elfArtifact
        generated = [ordered]@{
            xparameters = $xparametersArtifact
            linker_script = $linkerArtifact
            build_log = $buildLogArtifact
            build_config_file = $buildConfigArtifact
        }
    }
    hdl = [ordered]@{
        # Keep the original fields for older manifest readers.
        file      = $xsaArtifact.file
        sha256    = $xsaArtifact.sha256
        xsa       = $xsaArtifact
        bitstream = $bitArtifact
    }
    submodules   = $submodules
    build_config = $buildConfig
    build_config_provenance = if ($buildConfigArtifact.sha256) {
        'baseline values plus overrides in firmware.generated.build_config_file'
    } else {
        'clean-wrapper defaults; no local build configuration file'
    }
}

# --- Capture toolchain versions ---
$toolchain = [ordered]@{}

# Vivado
$vivadoPath = $env:XILINX_VIVADO
if (-not $vivadoPath) {
    $vivadoPath = (Get-Command vivado -ErrorAction SilentlyContinue | Select-Object -First 1).Source
}
if ($vivadoPath) {
    if ($vivadoPath -match '(\d{4}\.\d+)') { $toolchain['vivado'] = $Matches[1] }
    else { $toolchain['vivado'] = $vivadoPath }
} else {
    $toolchain['vivado'] = 'not found'
}

# Vitis / SDK
$vitisPath = $env:XILINX_VITIS
if (-not $vitisPath) {
    $vitisPath = (Get-Command xsct -ErrorAction SilentlyContinue | Select-Object -First 1).Source
}
if ($vitisPath) {
    if ($vitisPath -match '(\d{4}\.\d+)') { $toolchain['vitis'] = $Matches[1] }
    else { $toolchain['vitis'] = $vitisPath }
} else {
    $toolchain['vitis'] = 'not found'
}

# MicroBlaze GCC
$mbGcc = Get-Command mb-gcc -ErrorAction SilentlyContinue
if (-not $mbGcc) {
    $mbGcc = Get-Command microblaze-xilinx-elf-gcc -ErrorAction SilentlyContinue
}
if ($mbGcc) {
    $gccVer = (& $mbGcc.Source --version 2>$null | Select-Object -First 1)
    $toolchain['mb_gcc'] = if ($gccVer) { $gccVer.Trim() } else { $mbGcc.Source }
} else {
    $toolchain['mb_gcc'] = 'not found'
}

$manifest['toolchain'] = $toolchain

$outFile = [IO.Path]::GetFullPath($OutputPath)
$outDirectory = Split-Path -Parent $outFile
if (-not (Test-Path -LiteralPath $outDirectory -PathType Container)) {
    New-Item -ItemType Directory -Force -Path $outDirectory | Out-Null
}

if ($Verify) {
    # --- Verify mode ---
    if (-not (Test-Path -LiteralPath $outFile)) {
        Write-Error 'No manifest.json found. Run without -Verify first.'
        exit 1
    }
    $existing = Get-Content -LiteralPath $outFile -Raw | ConvertFrom-Json
    $mismatches = @()
    $warnings   = @()

    # Verify every requested artifact and the selected profile. The
    # firmware.commit is NOT checked here because
    # committing manifest.json itself creates a new commit, making the recorded
    # SHA perpetually one behind (circular dependency). Git log already tracks
    # which commit the manifest lives in — no need to duplicate that check.
    # Accept the old manifest shape while repositories migrate to the explicit
    # artifact records above.
    $recordedXsaHash = if ($existing.hdl.xsa) {
        $existing.hdl.xsa.sha256
    } else {
        $existing.hdl.sha256
    }
    if ($recordedXsaHash -ne $xsaArtifact.sha256) {
        $mismatches += ('XSA sha256: manifest={0} current={1}' -f
            $recordedXsaHash, $xsaArtifact.sha256)
    }
    if ($BitPath -and $existing.hdl.bitstream.sha256 -ne $bitArtifact.sha256) {
        $mismatches += ('bitstream sha256: manifest={0} current={1}' -f
            $existing.hdl.bitstream.sha256, $bitArtifact.sha256)
    }
    if ($ElfPath -and $existing.firmware.elf.sha256 -ne $elfArtifact.sha256) {
        $mismatches += ('ELF sha256: manifest={0} current={1}' -f
            $existing.firmware.elf.sha256, $elfArtifact.sha256)
    }
    foreach ($requested in @(
        @{ Path = $XparametersPath; Existing = $existing.firmware.generated.xparameters.sha256; Current = $xparametersArtifact.sha256; Label = 'xparameters.h' },
        @{ Path = $LinkerScriptPath; Existing = $existing.firmware.generated.linker_script.sha256; Current = $linkerArtifact.sha256; Label = 'linker script' },
        @{ Path = $BuildLogPath; Existing = $existing.firmware.generated.build_log.sha256; Current = $buildLogArtifact.sha256; Label = 'build log' },
        @{ Path = $BuildConfigPath; Existing = $existing.firmware.generated.build_config_file.sha256; Current = $buildConfigArtifact.sha256; Label = 'build configuration file' }
    )) {
        if ($requested.Path -and $requested.Existing -ne $requested.Current) {
            $mismatches += ('{0} sha256: manifest={1} current={2}' -f
                $requested.Label, $requested.Existing, $requested.Current)
        }
    }
    if ($Profile -ne 'unknown' -and $existing.firmware.profile -ne $Profile) {
        $mismatches += ('profile: manifest={0} current={1}' -f
            $existing.firmware.profile, $Profile)
    }
    if ($fwDirty) {
        $warnings += 'Working tree has uncommitted changes (dirty) - normal during development'
    }

    # Informational: show what commit the manifest was generated on
    $recordedCommit = [string]$existing.firmware.commit
    $infoCommit = if ([string]::IsNullOrWhiteSpace($recordedCommit)) {
        'unknown'
    } else {
        $recordedCommit.Substring(0, [Math]::Min(12, $recordedCommit.Length))
    }

    if ($mismatches.Count -eq 0) {
        Write-Host ('MANIFEST: Verification PASSED - requested inputs match (recorded on {0})' -f $infoCommit) -ForegroundColor Green
        foreach ($w in $warnings) {
            Write-Host ('  WARN: {0}' -f $w) -ForegroundColor Yellow
        }
    } else {
        Write-Host 'MANIFEST: Verification FAILED - requested inputs differ:' -ForegroundColor Red
        foreach ($m in $mismatches) {
            Write-Host ('  - {0}' -f $m) -ForegroundColor Yellow
        }
        Write-Host '  Run gen_manifest.ps1 to update the manifest.' -ForegroundColor Yellow
        exit 1
    }
} else {
    # --- Generate mode ---
    $json = $manifest | ConvertTo-Json -Depth 6
    Set-Content -LiteralPath $outFile -Value $json -Encoding UTF8
    Write-Host ('MANIFEST: Generated {0}' -f $outFile) -ForegroundColor Green
    Write-Host ('  firmware commit : {0}' -f $fwCommit) -ForegroundColor Cyan
    Write-Host ('  firmware branch : {0}' -f $fwBranch) -ForegroundColor Cyan
    Write-Host ('  profile         : {0}' -f $Profile) -ForegroundColor Cyan
    Write-Host ('  XSA sha256      : {0}' -f $xsaArtifact.sha256) -ForegroundColor Cyan
    if ($bitArtifact.sha256) {
        Write-Host ('  bit sha256      : {0}' -f $bitArtifact.sha256) -ForegroundColor Cyan
    }
    if ($elfArtifact.sha256) {
        Write-Host ('  ELF sha256      : {0}' -f $elfArtifact.sha256) -ForegroundColor Cyan
    }
    Write-Host ('  dirty           : {0}' -f $fwDirty) -ForegroundColor Cyan
}
