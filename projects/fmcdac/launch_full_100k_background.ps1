$ErrorActionPreference = "Stop"

$repoRoot = "C:\Users\fpga_\yr\tmp\no-OS-adi-fork"
$projectDir = Join-Path $repoRoot "projects\fmcdac"
$timestamp = Get-Date -Format "yyyyMMdd_HHmmss"
$parentRunDir = Join-Path $projectDir ("capture_runs\launch_100k_" + $timestamp)

$pythonExe = "python"
$serialPort = "COM4"
$visaResource = "TCPIP::192.168.100.142::INSTR"
$visaBackend = "@py"

$xilinxSettings = @(
    "C:\Xilinx\Vivado\2021.2\settings64.bat",
    "C:\Xilinx\Vitis_HLS\2021.2\settings64.bat",
    "C:\Xilinx\Vitis\2021.2\settings64.bat"
)

$awgRunDir = Join-Path $parentRunDir "awg_scheduler_pass"
$suiteRunDir = Join-Path $parentRunDir "full_suite_pass"

New-Item -ItemType Directory -Force -Path $awgRunDir | Out-Null
New-Item -ItemType Directory -Force -Path $suiteRunDir | Out-Null

$awgArgs = @(
    ".\projects\fmcdac\awg_sweep_test.py",
    "--serial-port", $serialPort,
    "--visa-resource", $visaResource,
    "--visa-backend", $visaBackend,
    "--xilinx-settings", $xilinxSettings[0],
    "--xilinx-settings", $xilinxSettings[1],
    "--xilinx-settings", $xilinxSettings[2],
    "--awg-sched-baseaddr", "0x44AA0000",
    "--awg-sweep-start-hz", "200000000",
    "--awg-sweep-stop-hz", "210000000",
    "--awg-sweep-step-hz", "1000000",
    "--output-dir", $awgRunDir
)

$suiteArgs = @(
    ".\projects\fmcdac\run_nco_scope_test.py",
    "--serial-port", $serialPort,
    "--visa-resource", $visaResource,
    "--visa-backend", $visaBackend,
    "--xilinx-settings", $xilinxSettings[0],
    "--xilinx-settings", $xilinxSettings[1],
    "--xilinx-settings", $xilinxSettings[2],
    "--run-nco-test",
    "--dds-band-sweep-start-hz", "200000000",
    "--dds-band-sweep-stop-hz", "300000000",
    "--dds-band-sweep-step-hz", "100000",
    "--sfdr-sweep-start-hz", "200000000",
    "--sfdr-sweep-stop-hz", "300000000",
    "--sfdr-sweep-step-hz", "100000",
    "--output-dir", $suiteRunDir
)

## Start AWG scheduler process first, wait for it to finish, then start the analyzer suite
$awgProcess = Start-Process `
    -FilePath $pythonExe `
    -WorkingDirectory $repoRoot `
    -ArgumentList $awgArgs `
    -RedirectStandardOutput (Join-Path $awgRunDir "host.stdout.log") `
    -RedirectStandardError (Join-Path $awgRunDir "host.stderr.log") `
    -PassThru

Set-Content -Path (Join-Path $awgRunDir "pid.txt") -Value $awgProcess.Id

Write-Host "Started AWG scheduler process, PID: $($awgProcess.Id)"
Write-Host "Waiting for AWG scheduler to complete..."
Wait-Process -Id $awgProcess.Id

# Start analyzer suite after AWG scheduler completes
$suiteProcess = Start-Process `
    -FilePath $pythonExe `
    -WorkingDirectory $repoRoot `
    -ArgumentList $suiteArgs `
    -RedirectStandardOutput (Join-Path $suiteRunDir "host.stdout.log") `
    -RedirectStandardError (Join-Path $suiteRunDir "host.stderr.log") `
    -PassThru

Set-Content -Path (Join-Path $suiteRunDir "pid.txt") -Value $suiteProcess.Id

$launcherSummary = [ordered]@{
    timestamp = $timestamp
    repo_root = $repoRoot
    parent_run_dir = $parentRunDir
    awg_scheduler = [ordered]@{
        pid = $awgProcess.Id
        run_dir = $awgRunDir
        stdout_log = (Join-Path $awgRunDir "host.stdout.log")
        stderr_log = (Join-Path $awgRunDir "host.stderr.log")
        command = @($pythonExe) + $awgArgs
    }
    full_suite = [ordered]@{
        pid = $suiteProcess.Id
        run_dir = $suiteRunDir
        stdout_log = (Join-Path $suiteRunDir "host.stdout.log")
        stderr_log = (Join-Path $suiteRunDir "host.stderr.log")
        command = @($pythonExe) + $suiteArgs
    }
}

$launcherSummaryPath = Join-Path $parentRunDir "launcher_summary.json"
$launcherSummary | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 $launcherSummaryPath

Write-Host ""
Write-Host "Started background runs (sequential)."
Write-Host "Parent run dir: $parentRunDir"
Write-Host ""
Write-Host "AWG scheduler:"
Write-Host "  PID: $($awgProcess.Id)"
Write-Host "  Stdout: $(Join-Path $awgRunDir 'host.stdout.log')"
Write-Host "  Stderr: $(Join-Path $awgRunDir 'host.stderr.log')"
Write-Host ""
Write-Host "Full analyzer suite:"
Write-Host "  PID: $($suiteProcess.Id)"
Write-Host "  Stdout: $(Join-Path $suiteRunDir 'host.stdout.log')"
Write-Host "  Stderr: $(Join-Path $suiteRunDir 'host.stderr.log')"
Write-Host ""
Write-Host "Launcher summary:"
Write-Host "  $launcherSummaryPath"
Write-Host ""
Write-Host "Track progress with:"
Write-Host "  Get-Content `"$($awgRunDir)\host.stdout.log`" -Wait"
Write-Host "  Get-Content `"$($suiteRunDir)\host.stdout.log`" -Wait"
Write-Host ""
Write-Host "Check running state with:"
Write-Host "  Get-Process -Id $($awgProcess.Id),$($suiteProcess.Id)"
