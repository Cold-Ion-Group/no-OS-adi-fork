<#
.SYNOPSIS
    Configure one Windows network adapter for the FMCDAC AWG link.

.DESCRIPTION
    Adds one IPv4 address and one on-link host route for the AWG target. The
    script does not change DNS, gateways, interface metrics, or unrelated
    routes. State is saved as JSON so -Remove deletes only items that this
    script added.

    Run this script from an elevated PowerShell session. Use -WhatIf to preview
    changes. The default state file is written to the current directory.

.EXAMPLE
    .\configure_awg_nic.ps1 -InterfaceAlias "Ethernet 2" -WhatIf

.EXAMPLE
    .\configure_awg_nic.ps1 -InterfaceAlias "Ethernet 2" `
        -StateFile D:\awg-runs\nic-state.json

.EXAMPLE
    .\configure_awg_nic.ps1 -InterfaceAlias "Ethernet 2" `
        -StateFile D:\awg-runs\nic-state.json -Remove
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = "Medium")]
param(
    [Parameter(Mandatory = $true)]
    [string]$InterfaceAlias,

    [string]$Address = "192.0.2.1",

    [ValidateRange(1, 32)]
    [int]$PrefixLength = 24,

    [string]$TargetAddress = "192.0.2.2",

    [string]$StateFile = "",

    [switch]$Remove
)

$ErrorActionPreference = "Stop"

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    $administrator = [Security.Principal.WindowsBuiltInRole]::Administrator

    if (-not $principal.IsInRole($administrator)) {
        throw "Run this script from an elevated PowerShell session."
    }
}

function ConvertTo-IPv4String {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Value,

        [Parameter(Mandatory = $true)]
        [string]$ParameterName
    )

    $parsed = $null
    if (-not [Net.IPAddress]::TryParse($Value, [ref]$parsed) -or
        $parsed.AddressFamily -ne [Net.Sockets.AddressFamily]::InterNetwork) {
        throw "$ParameterName must be an IPv4 address: $Value"
    }

    return $parsed.ToString()
}

function Resolve-StateFile {
    param(
        [string]$RequestedPath,
        [string]$Alias
    )

    if ([string]::IsNullOrWhiteSpace($RequestedPath)) {
        $safeAlias = $Alias -replace '[^A-Za-z0-9._-]', '_'
        $RequestedPath = Join-Path (Get-Location).Path "awg_nic_state_$safeAlias.json"
    }

    return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
        $RequestedPath
    )
}

function Write-StateFile {
    param(
        [Parameter(Mandatory = $true)]
        [object]$State,

        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    $parent = Split-Path -Parent $Path
    if ($parent -and -not (Test-Path -LiteralPath $parent)) {
        New-Item -ItemType Directory -Path $parent -Force | Out-Null
    }

    $State | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $Path -Encoding UTF8
}

Assert-Administrator

foreach ($command in @(
        "Get-NetAdapter",
        "Get-NetIPAddress",
        "New-NetIPAddress",
        "Remove-NetIPAddress",
        "Get-NetRoute",
        "New-NetRoute",
        "Remove-NetRoute"
    )) {
    if (-not (Get-Command $command -ErrorAction SilentlyContinue)) {
        throw "Required Windows networking command is unavailable: $command"
    }
}

$Address = ConvertTo-IPv4String -Value $Address -ParameterName "Address"
$TargetAddress = ConvertTo-IPv4String `
    -Value $TargetAddress `
    -ParameterName "TargetAddress"
$targetPrefix = "$TargetAddress/32"
$resolvedStateFile = Resolve-StateFile `
    -RequestedPath $StateFile `
    -Alias $InterfaceAlias

$adapter = Get-NetAdapter -Name $InterfaceAlias -ErrorAction Stop
if (@($adapter).Count -ne 1) {
    throw "InterfaceAlias must identify exactly one network adapter: $InterfaceAlias"
}
$interfaceIndex = [int]$adapter.ifIndex

if ($Remove) {
    if (-not (Test-Path -LiteralPath $resolvedStateFile)) {
        throw "State file not found: $resolvedStateFile"
    }

    $state = Get-Content -LiteralPath $resolvedStateFile -Raw | ConvertFrom-Json
    if ($state.interface_alias -ne $InterfaceAlias -or
        [int]$state.interface_index -ne $interfaceIndex) {
        throw "State file belongs to a different network adapter."
    }

    if (-not $state.active) {
        Write-Host "No active AWG NIC changes are recorded in $resolvedStateFile"
        return
    }

    $description = "managed AWG address and target route on '$InterfaceAlias'"
    if (-not $PSCmdlet.ShouldProcess($description, "Remove")) {
        return
    }

    if ($state.route_added_by_script) {
        $managedRoutes = Get-NetRoute `
            -AddressFamily IPv4 `
            -InterfaceIndex $interfaceIndex `
            -DestinationPrefix $state.target_prefix `
            -ErrorAction SilentlyContinue |
            Where-Object {
                $_.NextHop -eq "0.0.0.0" -and
                $_.RouteMetric -eq [int]$state.route_metric
            }

        foreach ($route in @($managedRoutes)) {
            Remove-NetRoute -InputObject $route -Confirm:$false
        }
    }

    if ($state.address_added_by_script) {
        $managedAddresses = Get-NetIPAddress `
            -AddressFamily IPv4 `
            -InterfaceIndex $interfaceIndex `
            -IPAddress $state.address `
            -ErrorAction SilentlyContinue |
            Where-Object { $_.PrefixLength -eq [int]$state.prefix_length }

        foreach ($managedAddress in @($managedAddresses)) {
            Remove-NetIPAddress -InputObject $managedAddress -Confirm:$false
        }
    }

    $state.active = $false
    $state.removed_utc = [DateTime]::UtcNow.ToString("o")
    Write-StateFile -State $state -Path $resolvedStateFile
    Write-Host "Removed recorded AWG NIC changes. State: $resolvedStateFile"
    return
}

$existingState = $null
if (Test-Path -LiteralPath $resolvedStateFile) {
    $existingState = Get-Content -LiteralPath $resolvedStateFile -Raw |
        ConvertFrom-Json
    if ($existingState.active) {
        throw "State file already records active changes. Run with -Remove first: $resolvedStateFile"
    }
}

$sameAddress = @(
    Get-NetIPAddress `
        -AddressFamily IPv4 `
        -InterfaceIndex $interfaceIndex `
        -IPAddress $Address `
        -ErrorAction SilentlyContinue
)
if ($sameAddress.Count -gt 0 -and
    @($sameAddress | Where-Object { $_.PrefixLength -eq $PrefixLength }).Count -eq 0) {
    throw "$Address already exists on '$InterfaceAlias' with a different prefix length."
}

$managedAddressExists = @(
    $sameAddress | Where-Object { $_.PrefixLength -eq $PrefixLength }
).Count -gt 0

$managedRouteExists = @(
    Get-NetRoute `
        -AddressFamily IPv4 `
        -InterfaceIndex $interfaceIndex `
        -DestinationPrefix $targetPrefix `
        -ErrorAction SilentlyContinue |
        Where-Object { $_.NextHop -eq "0.0.0.0" }
).Count -gt 0

$state = [ordered]@{
    schema_version          = 1
    created_utc             = [DateTime]::UtcNow.ToString("o")
    active                  = $true
    removed_utc             = $null
    interface_alias         = $InterfaceAlias
    interface_index         = $interfaceIndex
    interface_description   = $adapter.InterfaceDescription
    address                 = $Address
    prefix_length           = $PrefixLength
    target_address          = $TargetAddress
    target_prefix           = $targetPrefix
    route_metric            = 1
    address_added_by_script = -not $managedAddressExists
    route_added_by_script   = -not $managedRouteExists
    previous_ipv4_addresses = @(
        Get-NetIPAddress `
            -AddressFamily IPv4 `
            -InterfaceIndex $interfaceIndex `
            -ErrorAction SilentlyContinue |
            Select-Object IPAddress, PrefixLength, PrefixOrigin, SuffixOrigin,
                SkipAsSource
    )
    previous_target_routes  = @(
        Get-NetRoute `
            -AddressFamily IPv4 `
            -InterfaceIndex $interfaceIndex `
            -DestinationPrefix $targetPrefix `
            -ErrorAction SilentlyContinue |
            Select-Object DestinationPrefix, NextHop, RouteMetric, PolicyStore
    )
}

$description = "$Address/$PrefixLength and $targetPrefix on '$InterfaceAlias'"
if (-not $PSCmdlet.ShouldProcess($description, "Configure AWG network link")) {
    return
}

# Save the original state before changing the adapter.
Write-StateFile -State $state -Path $resolvedStateFile

if (-not $managedAddressExists) {
    New-NetIPAddress `
        -InterfaceIndex $interfaceIndex `
        -IPAddress $Address `
        -PrefixLength $PrefixLength `
        -AddressFamily IPv4 `
        -PolicyStore ActiveStore | Out-Null
}

if (-not $managedRouteExists) {
    New-NetRoute `
        -AddressFamily IPv4 `
        -InterfaceIndex $interfaceIndex `
        -DestinationPrefix $targetPrefix `
        -NextHop "0.0.0.0" `
        -RouteMetric 1 `
        -PolicyStore ActiveStore | Out-Null
}

Write-Host "AWG NIC configured on '$InterfaceAlias'."
Write-Host "State backup: $resolvedStateFile"
Write-Host "Only $Address/$PrefixLength and $targetPrefix were managed."
