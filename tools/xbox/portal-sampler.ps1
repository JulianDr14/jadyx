# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Samples the console's system counters through the Device Portal while a game runs, one CSV row
# per interval: system CPU load, jadyx.exe CPU and memory, per-engine GPU utilization, GPU memory,
# system commit and available memory, disk IO. gpu_engine0 follows the game's D3D12 queue (it
# matches the renderer's "GPU busy"); gpu_engine5 reads ~100% even with the game paused, so it is
# not the game's work. The view from outside the app, to set next to the
# app's own perf windows (perf-summary.py) and profiles (profile-report.py, hunt-report.py).
#
#   powershell -ExecutionPolicy Bypass -File tools\xbox\portal-sampler.ps1 [-Seconds 600]
#       [-Interval 1] [-Out build-uwp\perf-runs\<run>\portal.csv]
#
# Stops after -Seconds or with Ctrl+C; rows are flushed as they come. The portal's certificate is
# self-signed: curl.exe runs with -k.
param(
    [int] $Seconds = 600,
    [double] $Interval = 1,
    [string] $Out = '',
    [string] $Console = $(if ($env:JADYX_CONSOLE) { $env:JADYX_CONSOLE } else { '192.168.1.50' })
)
$ErrorActionPreference = 'Stop'
# Decimal points, not the locale's commas, or the CSV columns shift.
[Threading.Thread]::CurrentThread.CurrentCulture = [Globalization.CultureInfo]::InvariantCulture
$portal = "https://${Console}:11443"
if (!$Out) {
    $Out = Join-Path $PSScriptRoot "..\..\build-uwp\perf-runs\portal-$(Get-Date -Format 'MMdd-HHmm').csv"
}
New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null

function Get-Json([string] $path) {
    $text = & curl.exe -sSk --max-time 5 "$portal$path"
    if ($LASTEXITCODE -ne 0) { return $null }
    return ($text | Out-String | ConvertFrom-Json)
}

$writer = [IO.StreamWriter]::new($Out, $false, [Text.UTF8Encoding]::new($false))
$header = $null
$start = Get-Date
try {
    while (((Get-Date) - $start).TotalSeconds -lt $Seconds) {
        $tick = Get-Date
        $perf = Get-Json '/api/resourcemanager/systemperf'
        $procs = Get-Json '/api/resourcemanager/processes'
        if ($perf) {
            $gpu = $perf.GPUData.AvailableAdapters | Select-Object -First 1
            $app = $procs.Processes | Where-Object { $_.ImageName -eq 'jadyx.exe' } | Select-Object -First 1
            $engines = @($gpu.EnginesUtilization)
            if (!$header) {
                $header = @('t_s', 'cpu_pct', 'app_cpu_pct', 'app_ws_mib', 'app_commit_mib',
                            'commit_mib', 'commit_limit_mib', 'available_mib',
                            'gpu_dedicated_mib', 'gpu_system_mib', 'io_read_mbs', 'io_write_mbs') +
                          (0..($engines.Count - 1) | ForEach-Object { "gpu_engine$_" })
                $writer.WriteLine($header -join ',')
            }
            $page = $perf.PageSize / 1MB
            $row = @(
                [math]::Round(($tick - $start).TotalSeconds, 1),
                $perf.CpuLoad,
                $(if ($app) { [math]::Round($app.CPUUsage, 1) } else { '' }),
                $(if ($app) { [math]::Round($app.PrivateWorkingSet / 1MB) } else { '' }),
                $(if ($app) { [math]::Round($app.TotalCommit / 1MB) } else { '' }),
                [math]::Round($perf.CommittedPages * $page),
                [math]::Round($perf.CommitLimit * $page),
                [math]::Round($perf.AvailablePages * $page),
                [math]::Round($gpu.DedicatedMemoryUsed / 1MB),
                [math]::Round($gpu.SystemMemoryUsed / 1MB),
                [math]::Round($perf.IOReadSpeed / 1MB, 2),
                [math]::Round($perf.IOWriteSpeed / 1MB, 2)
            ) + ($engines | ForEach-Object { [math]::Round($_, 1) })
            $writer.WriteLine(($row | ForEach-Object { [string]::Format(
                [Globalization.CultureInfo]::InvariantCulture, '{0}', $_) }) -join ',')
            $writer.Flush()
        }
        $left = $Interval - ((Get-Date) - $tick).TotalSeconds
        if ($left -gt 0) { Start-Sleep -Milliseconds ([int]($left * 1000)) }
    }
} finally {
    $writer.Dispose()
    Write-Host "portal samples -> $((Resolve-Path $Out).Path)"
}
