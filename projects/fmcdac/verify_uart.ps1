<#
.SYNOPSIS
    Verify a UART boot log against golden reference patterns.

.DESCRIPTION
    Parses a UART log file (or reads from clipboard) and checks for the
    presence of all required pass-criteria strings from the golden boot.
    Returns exit code 0 on full pass, 1 on any failure or missing pattern.

.EXAMPLE
    .\verify_uart.ps1 -LogFile .\captured_uart.txt
    .\verify_uart.ps1 -LogFile .\captured_uart.txt -GoldenFile .\golden\uart_boot_log.txt
#>
param(
    [Parameter(Mandatory=$true)]
    [string]$LogFile,

    [string]$GoldenFile = ''
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $LogFile)) {
    Write-Error "Log file not found: $LogFile"
    exit 1
}

$log = Get-Content $LogFile -Raw
if ([string]::IsNullOrWhiteSpace($log)) {
    Write-Error "Log file is empty: $LogFile"
    exit 1
}

# --- Required pass criteria ---
$criteria = @(
    [pscustomobject]@{Tag='LINK';  Pat='All lanes in sync (CGS+Frame+Checksum+ILAS)'; Desc='JESD link-up all 4 lanes'}
    [pscustomobject]@{Tag='STPL';  Pat='STPL PASS dac=0 sample=0';                    Desc='STPL DAC0 sample 0'}
    [pscustomobject]@{Tag='STPL';  Pat='STPL PASS dac=0 sample=1';                    Desc='STPL DAC0 sample 1'}
    [pscustomobject]@{Tag='STPL';  Pat='STPL PASS dac=1 sample=0';                    Desc='STPL DAC1 sample 0'}
    [pscustomobject]@{Tag='STPL';  Pat='STPL PASS dac=1 sample=1';                    Desc='STPL DAC1 sample 1'}
    [pscustomobject]@{Tag='PRBS';  Pat='PRBS7 test PASSED';                            Desc='PRBS-7 datapath'}
    [pscustomobject]@{Tag='PRBS';  Pat='PRBS15 test PASSED';                           Desc='PRBS-15 datapath'}
    [pscustomobject]@{Tag='DDS';   Pat='Starting frequency sweep';                     Desc='DDS sweep started'}
    [pscustomobject]@{Tag='DDS';   Pat='Done. Holding max frequency';                  Desc='DDS sweep completed'}
    [pscustomobject]@{Tag='FINAL'; Pat='All tests PASSED';                             Desc='Overall pass verdict'}
)

$pass = 0
$fail = 0
$total = $criteria.Count

Write-Host ''
Write-Host ('Checking {0} criteria in: {1}' -f $total, $LogFile)
Write-Host ('-' * 60)

foreach ($c in $criteria) {
    if ($log.Contains($c.Pat)) {
        Write-Host ('  PASS  [{0,-5}] {1}' -f $c.Tag, $c.Desc) -ForegroundColor Green
        $pass++
    } else {
        Write-Host ('  FAIL  [{0,-5}] {1}' -f $c.Tag, $c.Desc) -ForegroundColor Red
        Write-Host ('         missing: "{0}"' -f $c.Pat) -ForegroundColor Yellow
        $fail++
    }
}

Write-Host ('-' * 60)

# --- Optional golden diff ---
if ($GoldenFile -and (Test-Path $GoldenFile)) {
    $golden = Get-Content $GoldenFile -Raw

    # Compare key register values
    $regPatterns = @(
        'CGS\s+\(0x470\)\s*=\s*(0x[0-9A-Fa-f]+)',
        'Frame Sync\(0x471\)\s*=\s*(0x[0-9A-Fa-f]+)',
        'Checksum\s+\(0x472\)\s*=\s*(0x[0-9A-Fa-f]+)',
        'XBAR0 \(0x308\)\s*=\s*(0x[0-9A-Fa-f]+)',
        'Lane invert \(0x334\)\s*=\s*(0x[0-9A-Fa-f]+)'
    )

    $regMismatches = 0
    foreach ($rp in $regPatterns) {
        $goldenMatch = [regex]::Match($golden, $rp)
        $logMatch    = [regex]::Match($log, $rp)
        if ($goldenMatch.Success -and $logMatch.Success) {
            if ($goldenMatch.Groups[1].Value -ne $logMatch.Groups[1].Value) {
                Write-Host ('  DRIFT  {0}: golden={1} current={2}' -f $rp,
                    $goldenMatch.Groups[1].Value,
                    $logMatch.Groups[1].Value) -ForegroundColor Yellow
                $regMismatches++
            }
        }
    }
    if ($regMismatches -eq 0) {
        Write-Host '  Golden register comparison: all match' -ForegroundColor Green
    } else {
        Write-Host ('  Golden register comparison: {0} drift(s) detected' -f $regMismatches) -ForegroundColor Yellow
        $fail += $regMismatches
    }

    Write-Host ('-' * 60)
}

# --- Summary ---
if ($fail -eq 0) {
    Write-Host ('RESULT: PASS ({0}/{1} criteria met)' -f $pass, $total) -ForegroundColor Green
    exit 0
} else {
    Write-Host ('RESULT: FAIL ({0} passed, {1} failed)' -f $pass, $fail) -ForegroundColor Red
    exit 1
}
