<#
.SYNOPSIS
    Generate manifest.json for FMCDAC build reproducibility.

.DESCRIPTION
    Captures exact git commit SHAs, submodule states, XSA file hash,
    and JESD build configuration into a single manifest.json file.
    Run from the projects/fmcdac/ directory.

.EXAMPLE
    .\gen_manifest.ps1
    .\gen_manifest.ps1 -Verify
#>
param(
    [switch]$Verify
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
$subRaw = git -C $RepoRoot submodule status 2>$null
if ($subRaw) {
    foreach ($line in $subRaw) {
        if ($line -match '^\s*[+-]?([0-9a-f]+)\s+(\S+)') {
            $submodules[$Matches[2]] = $Matches[1]
        }
    }
}

# --- Hash the XSA file ---
$xsaFile = Join-Path $ProjectDir 'system_top.xsa'
$xsaHash = ''
if (Test-Path $xsaFile) {
    $xsaHash = (Get-FileHash -Algorithm SHA256 -Path $xsaFile).Hash.ToLower()
} else {
    Write-Warning 'system_top.xsa not found - XSA hash will be empty.'
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
    created   = (Get-Date -Format 'yyyy-MM-ddTHH:mm:ssK')
    firmware  = [ordered]@{
        repo     = 'Cold-Ion-Group/no-OS-adi-fork'
        branch   = $fwBranch
        commit   = $fwCommit
        describe = $fwDescribe
        dirty    = $fwDirty
    }
    hdl       = [ordered]@{
        file     = 'system_top.xsa'
        sha256   = $xsaHash
    }
    submodules   = $submodules
    build_config = $buildConfig
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

$outFile = Join-Path $ProjectDir 'manifest.json'

if ($Verify) {
    # --- Verify mode ---
    if (-not (Test-Path $outFile)) {
        Write-Error 'No manifest.json found. Run without -Verify first.'
        exit 1
    }
    $existing = Get-Content $outFile -Raw | ConvertFrom-Json
    $mismatches = @()
    $warnings   = @()

    # The XSA hash is the only thing worth verifying — it lives outside git,
    # so git can't track it. The firmware.commit is NOT checked here because
    # committing manifest.json itself creates a new commit, making the recorded
    # SHA perpetually one behind (circular dependency). Git log already tracks
    # which commit the manifest lives in — no need to duplicate that check.
    if ($existing.hdl.sha256 -ne $xsaHash) {
        $mismatches += ('hdl.sha256 (XSA): manifest={0} current={1}' -f $existing.hdl.sha256, $xsaHash)
    }
    if ($fwDirty) {
        $warnings += 'Working tree has uncommitted changes (dirty) - normal during development'
    }

    # Informational: show what commit the manifest was generated on
    $infoCommit = if ($existing.firmware.commit) { $existing.firmware.commit.Substring(0,12) } else { 'unknown' }

    if ($mismatches.Count -eq 0) {
        Write-Host ('MANIFEST: Verification PASSED - XSA matches manifest (recorded on {0})' -f $infoCommit) -ForegroundColor Green
        foreach ($w in $warnings) {
            Write-Host ('  WARN: {0}' -f $w) -ForegroundColor Yellow
        }
    } else {
        Write-Host 'MANIFEST: Verification FAILED - XSA has changed since manifest was recorded:' -ForegroundColor Red
        foreach ($m in $mismatches) {
            Write-Host ('  - {0}' -f $m) -ForegroundColor Yellow
        }
        Write-Host '  Run gen_manifest.ps1 to update the manifest.' -ForegroundColor Yellow
        exit 1
    }
} else {
    # --- Generate mode ---
    $json = $manifest | ConvertTo-Json -Depth 4
    Set-Content -Path $outFile -Value $json -Encoding UTF8
    Write-Host ('MANIFEST: Generated {0}' -f $outFile) -ForegroundColor Green
    Write-Host ('  firmware commit : {0}' -f $fwCommit) -ForegroundColor Cyan
    Write-Host ('  firmware branch : {0}' -f $fwBranch) -ForegroundColor Cyan
    Write-Host ('  XSA sha256      : {0}' -f $xsaHash) -ForegroundColor Cyan
    Write-Host ('  dirty           : {0}' -f $fwDirty) -ForegroundColor Cyan
}
