param(
    [switch]$Inner
)

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Set-Location $repoRoot

$outputDir = Join-Path $repoRoot "projects\fmcdac\capture_runs\full_integration_pass"
New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$schedulerOutputDir = Join-Path $outputDir "scheduler_suite_pass"
$ddsOutputDir = Join-Path $outputDir "dds_suite_pass"

$commandScheduler = @(
    "python",
    ".\projects\fmcdac\run_nco_scope_test.py",
    "--serial-port", "COM4",
    "--visa-resource", "TCPIP::192.168.100.142::INSTR",
    "--visa-backend", "@py",
    "--xilinx-settings", "C:\Xilinx\Vivado\2021.2\settings64.bat",
    "--xilinx-settings", "C:\Xilinx\Vitis_HLS\2021.2\settings64.bat",
    "--xilinx-settings", "C:\Xilinx\Vitis\2021.2\settings64.bat",
    "--run-scheduler-benchmark-suite",
    "--awg-sched-baseaddr", "0x44AA0000",
    "--awg-sweep-start-hz", "200000000",
    "--awg-sweep-stop-hz", "210000000",
    "--awg-sweep-step-hz", "1000000",
    "--output-dir", $schedulerOutputDir
)

$commandDds = @(
    "python",
    ".\projects\fmcdac\run_nco_scope_test.py",
    "--serial-port", "COM4",
    "--visa-resource", "TCPIP::192.168.100.142::INSTR",
    "--visa-backend", "@py",
    "--xilinx-settings", "C:\Xilinx\Vivado\2021.2\settings64.bat",
    "--xilinx-settings", "C:\Xilinx\Vitis_HLS\2021.2\settings64.bat",
    "--xilinx-settings", "C:\Xilinx\Vitis\2021.2\settings64.bat",
    "--skip-nco-test",
    "--skip-throughput-test",
    "--skip-uart-rtt",
    "--output-dir", $ddsOutputDir
)

$stdoutLog = Join-Path $outputDir "host.stdout.log"
$stderrLog = Join-Path $outputDir "host.stderr.log"
$schedulerStdout = Join-Path $schedulerOutputDir "host.stdout.log"
$schedulerStderr = Join-Path $schedulerOutputDir "host.stderr.log"
$ddsStdout = Join-Path $ddsOutputDir "host.stdout.log"
$ddsStderr = Join-Path $ddsOutputDir "host.stderr.log"

if (-not $Inner) {
    $innerArgs = @(
        "-NoProfile",
        "-ExecutionPolicy", "Bypass",
        "-File", $PSCommandPath,
        "-Inner"
    )

    $process = Start-Process `
        -FilePath "powershell" `
        -ArgumentList $innerArgs `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $stdoutLog `
        -RedirectStandardError $stderrLog `
        -PassThru

    $launcherSummary = [ordered]@{
        pid = $process.Id
        stdout_log = $stdoutLog
        stderr_log = $stderrLog
        command = @("powershell") + $innerArgs
        scheduler = [ordered]@{
            output_dir = $schedulerOutputDir
            stdout_log = $schedulerStdout
            stderr_log = $schedulerStderr
            command = $commandScheduler
        }
        dds_suite = [ordered]@{
            output_dir = $ddsOutputDir
            stdout_log = $ddsStdout
            stderr_log = $ddsStderr
            command = $commandDds
        }
    }

    $launcherSummaryPath = Join-Path $outputDir "launcher_summary.json"
    $launcherSummary | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 $launcherSummaryPath

    Write-Host "Started background run."
    Write-Host "PID: $($process.Id)"
    Write-Host "Stdout: $stdoutLog"
    Write-Host "Stderr: $stderrLog"
    Write-Host "Summary: $launcherSummaryPath"
    return
}

New-Item -ItemType Directory -Force -Path $schedulerOutputDir | Out-Null
New-Item -ItemType Directory -Force -Path $ddsOutputDir | Out-Null

function Invoke-LoggedProcess {
    param(
        [string[]]$Command,
        [string]$StdoutLog,
        [string]$StderrLog,
        [string]$Label
    )

    Write-Host "[HOST] Starting $Label..."
    $process = Start-Process `
        -FilePath $Command[0] `
        -ArgumentList $Command[1..($Command.Count - 1)] `
        -WorkingDirectory $repoRoot `
        -RedirectStandardOutput $StdoutLog `
        -RedirectStandardError $StderrLog `
        -PassThru

    Wait-Process -Id $process.Id
    $process.Refresh()
    $exitCode = $process.ExitCode
    Write-Host "[HOST] $Label exit code: $exitCode"
    return $exitCode
}

Write-Host "[HOST] Skipping scheduler sweep (user requested manual run)."

$exitCode = Invoke-LoggedProcess `
    -Command $commandDds `
    -StdoutLog $ddsStdout `
    -StderrLog $ddsStderr `
    -Label "DDS analog suite"
exit $exitCode
