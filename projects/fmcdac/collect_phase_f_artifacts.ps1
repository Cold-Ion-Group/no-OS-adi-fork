<#
.SYNOPSIS
Collect one matching HDL, firmware, and runtime artifact bundle.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)] [string]$ArtifactRoot,
    [Parameter(Mandatory = $true)] [string]$HdlRepo,
    [Parameter(Mandatory = $true)] [string]$FirmwareRepo,
    [Parameter(Mandatory = $true)] [string]$XsaPath,
    [Parameter(Mandatory = $true)] [string]$BitPath,
    [Parameter(Mandatory = $true)] [string]$ElfPath,
    [Parameter(Mandatory = $true)] [string]$XparametersPath,
    [Parameter(Mandatory = $true)] [string]$LinkerScriptPath,
    [Parameter(Mandatory = $true)] [string]$BuildLogPath,
    [Parameter(Mandatory = $true)] [string]$HdlManifestPath,
    [Parameter(Mandatory = $true)] [string]$FirmwareManifestPath,
    [ValidateSet('default', 'scheduler-preload', 'scheduler-stream',
                 'scheduler-dma', 'scheduler-eth')]
    [string]$Profile = 'scheduler-eth',
    [string]$BuildConfigPath = '',
    [string[]]$UartLog = @(),
    [string[]]$UartRaw = @(),
    [string[]]$ProgramLog = @(),
    [string[]]$HostTelemetry = @()
)

$ErrorActionPreference = 'Stop'

function Resolve-RequiredFile {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-RequiredDirectory {
    param([string]$Path, [string]$Label)
    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        throw "$Label not found: $Path"
    }
    return (Resolve-Path -LiteralPath $Path).Path
}

function Resolve-RequiredGitRepo {
    param([string]$Path, [string]$Label)

    $repo = Resolve-RequiredDirectory $Path $Label
    $topOutput = @(& git -C $repo rev-parse --show-toplevel 2>$null)
    if ($LASTEXITCODE -ne 0 -or $topOutput.Count -eq 0) {
        throw "$Label is not a Git repository: $repo"
    }
    $top = (Resolve-Path -LiteralPath ($topOutput -join '')).Path
    if (-not $top.Equals($repo, [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label must be the repository root: $repo"
    }
    return $repo
}

function Assert-ExternalPath {
    param([string]$Path, [string[]]$ForbiddenRoots)

    foreach ($root in $ForbiddenRoots) {
        $prefix = $root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
        if ($Path.Equals($root, [StringComparison]::OrdinalIgnoreCase) -or
            $Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
            throw "-ArtifactRoot must be outside both Git worktrees: $Path"
        }
    }
}

function Assert-ArtifactHash {
    param([string]$Path, [string]$ExpectedHash, [string]$Label)

    if ([string]::IsNullOrWhiteSpace($ExpectedHash)) {
        throw "$Label manifest hash is missing."
    }
    $actual = (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash
    if (-not $actual.Equals($ExpectedHash,
            [StringComparison]::OrdinalIgnoreCase)) {
        throw "$Label does not match its manifest: $Path"
    }
}

function Get-GitRecord {
    param([string]$Repo)
    return [ordered]@{
        path = $Repo
        branch = (& git -C $Repo rev-parse --abbrev-ref HEAD).Trim()
        commit = (& git -C $Repo rev-parse HEAD).Trim()
        describe = (& git -C $Repo describe --all --long --dirty=-modified).Trim()
        submodules = @(& git -C $Repo submodule status --recursive)
    }
}

function Copy-OptionalFiles {
    param(
        [string[]]$Paths,
        [string]$Label,
        [string]$DestinationDirectory
    )

    foreach ($path in @($Paths)) {
        if ([string]::IsNullOrWhiteSpace($path)) {
            continue
        }
        $source = Resolve-RequiredFile $path $Label
        $destination = Join-Path $DestinationDirectory `
            ([IO.Path]::GetFileName($source))
        if (Test-Path -LiteralPath $destination) {
            throw "$Label file name is duplicated in the bundle: $destination"
        }
        Copy-Item -LiteralPath $source -Destination $destination
    }
}

if (-not [IO.Path]::IsPathRooted($ArtifactRoot)) {
    throw '-ArtifactRoot must be an absolute path outside the Git worktrees.'
}

$hdl = Resolve-RequiredGitRepo $HdlRepo 'HDL repository'
$firmware = Resolve-RequiredGitRepo $FirmwareRepo 'Firmware repository'
$xsa = Resolve-RequiredFile $XsaPath 'XSA'
$bit = Resolve-RequiredFile $BitPath 'Bitstream'
$elf = Resolve-RequiredFile $ElfPath 'ELF'
$xparameters = Resolve-RequiredFile $XparametersPath 'Generated xparameters.h'
$linkerScript = Resolve-RequiredFile $LinkerScriptPath 'Generated linker script'
$buildLog = Resolve-RequiredFile $BuildLogPath 'Firmware build log'
$hdlManifestFile = Resolve-RequiredFile $HdlManifestPath 'HDL build manifest'
$firmwareManifestFile = Resolve-RequiredFile `
    $FirmwareManifestPath 'Firmware build manifest'

$hdlManifest = Get-Content -LiteralPath $hdlManifestFile -Raw | ConvertFrom-Json
$firmwareManifest = Get-Content -LiteralPath $firmwareManifestFile -Raw |
    ConvertFrom-Json
$hdlHead = (@(& git -C $hdl rev-parse HEAD) -join '').Trim()
$firmwareHead = (@(& git -C $firmware rev-parse HEAD) -join '').Trim()
if ($hdlHead -ne [string]$hdlManifest.repository.head) {
    throw 'HDL repository HEAD does not match the HDL build manifest.'
}
if ($firmwareHead -ne [string]$firmwareManifest.firmware.commit) {
    throw 'Firmware repository HEAD does not match the firmware build manifest.'
}
if (@($hdlManifest.repository.dirty_status).Count -gt 0 -or
    [bool]$firmwareManifest.firmware.dirty) {
    throw 'Closure bundles require manifests generated from clean source trees.'
}
$hdlDirtyNow = @(& git -C $hdl status --porcelain)
$firmwareDirtyNow = @(& git -C $firmware status --porcelain)
if ($hdlDirtyNow.Count -gt 0 -or $firmwareDirtyNow.Count -gt 0) {
    throw 'Commit or stash current source changes before collecting a closure bundle.'
}
$hdlXsaRecord = @($hdlManifest.artifacts | Where-Object { $_.role -eq 'xsa' })
$hdlBitRecord = @($hdlManifest.artifacts |
    Where-Object { $_.role -eq 'bitstream' })
if ($hdlXsaRecord.Count -ne 1 -or $hdlBitRecord.Count -ne 1) {
    throw 'HDL manifest must contain exactly one XSA and one bitstream record.'
}
Assert-ArtifactHash $xsa ([string]$hdlXsaRecord[0].sha256) 'HDL XSA'
Assert-ArtifactHash $bit ([string]$hdlBitRecord[0].sha256) 'HDL bitstream'

$firmwareXsaHash = if ($firmwareManifest.hdl.xsa) {
    [string]$firmwareManifest.hdl.xsa.sha256
} else {
    [string]$firmwareManifest.hdl.sha256
}
Assert-ArtifactHash $xsa $firmwareXsaHash 'Firmware XSA'
Assert-ArtifactHash $elf ([string]$firmwareManifest.firmware.elf.sha256) `
    'Firmware ELF'
Assert-ArtifactHash $xparameters `
    ([string]$firmwareManifest.firmware.generated.xparameters.sha256) `
    'Generated xparameters.h'
Assert-ArtifactHash $linkerScript `
    ([string]$firmwareManifest.firmware.generated.linker_script.sha256) `
    'Generated linker script'
Assert-ArtifactHash $buildLog `
    ([string]$firmwareManifest.firmware.generated.build_log.sha256) `
    'Firmware build log'
$buildConfig = $null
$firmwareBuildConfigHash = `
    [string]$firmwareManifest.firmware.generated.build_config_file.sha256
if (-not [string]::IsNullOrWhiteSpace($firmwareBuildConfigHash)) {
    if ([string]::IsNullOrWhiteSpace($BuildConfigPath)) {
        throw 'The firmware manifest records a custom build file. Pass -BuildConfigPath.'
    }
    $buildConfig = Resolve-RequiredFile $BuildConfigPath `
        'Firmware build configuration file'
    Assert-ArtifactHash $buildConfig $firmwareBuildConfigHash `
        'Firmware build configuration file'
} elseif (-not [string]::IsNullOrWhiteSpace($BuildConfigPath)) {
    throw 'A build configuration file was supplied, but the firmware manifest does not record one.'
}
if ([string]$firmwareManifest.firmware.profile -ne $Profile) {
    throw "Firmware manifest profile does not match -Profile $Profile."
}

$resolvedArtifactRoot = [IO.Path]::GetFullPath($ArtifactRoot)
Assert-ExternalPath $resolvedArtifactRoot @($hdl, $firmware)
if (-not (Test-Path -LiteralPath $resolvedArtifactRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $resolvedArtifactRoot -Force | Out-Null
}
$stamp = [DateTime]::UtcNow.ToString('yyyyMMdd_HHmmss_fff')
$bundle = Join-Path $resolvedArtifactRoot "${stamp}_${Profile}"
if (Test-Path -LiteralPath $bundle) {
    throw "Artifact bundle already exists: $bundle"
}
$hdlOut = Join-Path $bundle 'hdl'
$firmwareOut = Join-Path $bundle 'firmware'
$runtimeOut = Join-Path $bundle 'runtime'
$hostOut = Join-Path $bundle 'host'
New-Item -ItemType Directory -Path $bundle | Out-Null
New-Item -ItemType Directory -Path $hdlOut, $firmwareOut, $runtimeOut, $hostOut | Out-Null

Copy-Item -LiteralPath $xsa, $bit -Destination $hdlOut -Force
Copy-Item -LiteralPath $elf -Destination $firmwareOut -Force
Copy-Item -LiteralPath $xparameters, $linkerScript, $buildLog `
    -Destination $firmwareOut -Force
if ($buildConfig) {
    Copy-Item -LiteralPath $buildConfig -Destination $firmwareOut -Force
}
Copy-Item -LiteralPath $hdlManifestFile `
    -Destination (Join-Path $hdlOut 'build_manifest.json') -Force
Copy-Item -LiteralPath $firmwareManifestFile `
    -Destination (Join-Path $firmwareOut 'build_manifest.json') -Force

foreach ($abi in @(
    @{ Role = 'scheduler_abi'; Label = 'Scheduler ABI header' },
    @{ Role = 'extension_abi'; Label = 'Extension ABI header' }
)) {
    $records = @($hdlManifest.artifacts |
        Where-Object { $_.role -eq $abi.Role })
    if ($records.Count -ne 1) {
        throw "HDL manifest must contain exactly one $($abi.Role) record."
    }
    $abiFile = Resolve-RequiredFile ([string]$records[0].path) $abi.Label
    Assert-ArtifactHash $abiFile ([string]$records[0].sha256) $abi.Label
    Copy-Item -LiteralPath $abiFile -Destination $hdlOut -Force
}

$hdlLogOut = Join-Path $hdlOut 'phase_e_logs'
$hdlLogRecords = @(
    $hdlManifest.artifacts | Where-Object { $_.role -eq 'log_or_report' }
)
if ($hdlLogRecords.Count -gt 0) {
    New-Item -ItemType Directory -Path $hdlLogOut | Out-Null
    foreach ($record in $hdlLogRecords) {
        $log = Resolve-RequiredFile ([string]$record.path) 'HDL log/report'
        Assert-ArtifactHash $log ([string]$record.sha256) 'HDL log/report'
        Copy-Item -LiteralPath $log -Destination $hdlLogOut -Force
    }
}

$fmcdac = Join-Path $firmware 'projects\fmcdac'

Copy-OptionalFiles -Paths $UartLog -Label 'UART log' `
    -DestinationDirectory $runtimeOut
Copy-OptionalFiles -Paths $UartRaw -Label 'Raw UART log' `
    -DestinationDirectory $runtimeOut
Copy-OptionalFiles -Paths $ProgramLog -Label 'Programming log' `
    -DestinationDirectory $runtimeOut
Copy-OptionalFiles -Paths $HostTelemetry -Label 'Host telemetry' `
    -DestinationDirectory $hostOut

$manifestScript = Join-Path $fmcdac 'gen_manifest.ps1'
$bundleManifest = Join-Path $bundle 'manifest.json'
$manifestArguments = @{
    XsaPath = $xsa
    BitPath = $bit
    ElfPath = $elf
    XparametersPath = $xparameters
    LinkerScriptPath = $linkerScript
    BuildLogPath = $buildLog
    Profile = $Profile
    OutputPath = $bundleManifest
}
if ($buildConfig) {
    $manifestArguments['BuildConfigPath'] = $buildConfig
}
& $manifestScript @manifestArguments
if (-not $?) {
    throw 'Bundle manifest generation failed.'
}

$source = [ordered]@{
    created = [DateTime]::UtcNow.ToString('o')
    profile = $Profile
    hdl = Get-GitRecord $hdl
    firmware = Get-GitRecord $firmware
    tools = [ordered]@{
        vivado = if (Get-Command vivado -ErrorAction SilentlyContinue) {
            (& vivado -version 2>$null | Select-Object -First 1)
        } else { 'not found' }
        xsct = if (Get-Command xsct -ErrorAction SilentlyContinue) {
            (& xsct -version 2>$null | Select-Object -First 1)
        } else { 'not found' }
        microblaze_gcc = if (Get-Command mb-gcc -ErrorAction SilentlyContinue) {
            (& mb-gcc --version 2>$null | Select-Object -First 1)
        } else { 'not found' }
    }
}
$source | ConvertTo-Json -Depth 6 |
    Set-Content -LiteralPath (Join-Path $bundle 'source.json') -Encoding UTF8

$hashLines = foreach ($file in Get-ChildItem -LiteralPath $bundle -Recurse -File |
                      Where-Object { $_.Name -ne 'SHA256SUMS' }) {
    $relative = $file.FullName.Substring($bundle.Length + 1).Replace('\', '/')
    $hash = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLower()
    "$hash  $relative"
}
$hashLines | Sort-Object |
    Set-Content -LiteralPath (Join-Path $bundle 'SHA256SUMS') -Encoding ASCII

Write-Host "Phase F artifact bundle: $bundle"
