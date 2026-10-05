# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Automated, guest-free AppContainer gate. Uses the existing incremental build; never stops gameplay.
[CmdletBinding()]
param(
    [switch] $GpuValidation,
    [switch] $GuestIntegration,
    [ValidateRange(60, 1800)] [int] $TimeoutSec = 240,
    [string] $OutputDirectory = 'build-uwp/astc-probe-results'
)
$ErrorActionPreference = 'Stop'
$taskRepo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
if (Get-Process -Name eden-uwp -ErrorAction SilentlyContinue) {
    throw 'eden-uwp is already running. Close it before starting the isolated probe.'
}
if (-not [IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputDirectory = Join-Path $taskRepo $OutputDirectory
}
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$destination = Join-Path $OutputDirectory ($stamp + $(if ($GpuValidation) { '-gbv' } else { '-debug' }))
New-Item -ItemType Directory -Path $destination -Force | Out-Null
$started = [DateTime]::UtcNow
$deadline = $started.AddSeconds($TimeoutSec)
$peakCommit = 0L
$maxSampledCommit = 0L
$peakWorkingSet = 0L
$sampleCount = 0
$probePid = $null
$probeStartedAt = $null
$failure = $null
$job = Start-Job -ArgumentList $taskRepo, $TimeoutSec, ([bool]$GpuValidation), ([bool]$GuestIntegration) -ScriptBlock {
    param($repo, $timeout, $gbv, $guest)
    Set-Location -LiteralPath $repo
    $cfg = @('astc_sampling_probe=1')
    if ($guest) { $cfg += 'astc_guest_sampling=1' }
    $cfg += $(if ($gbv) { 'debug_layer=gbv' } else { 'debug_layer=1' })
    & (Join-Path $repo 'tools/xbox/local-run.ps1') -NoBuild -TimeoutSec $timeout -BootCfg $cfg
}
try {
    while ($job.State -eq 'Running') {
        $process = Get-Process -Name eden-uwp -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($process) {
            if ($null -ne $probePid -and $probePid -ne $process.Id) {
                throw 'Unexpected eden-uwp process replacement during probe.'
            }
            $probePid = $process.Id
            $probeStartedAt = $process.StartTime
            $peakCommit = [Math]::Max($peakCommit, $process.PeakPagedMemorySize64)
            $maxSampledCommit = [Math]::Max($maxSampledCommit, $process.PagedMemorySize64)
            $peakWorkingSet = [Math]::Max($peakWorkingSet, $process.PeakWorkingSet64)
            $sampleCount++
        }
        if ([DateTime]::UtcNow -gt $deadline) { throw "Probe timed out after $TimeoutSec seconds." }
        Start-Sleep -Milliseconds 200
    }
    Receive-Job -Job $job -ErrorAction Stop | Out-File (Join-Path $destination 'launcher.txt') -Encoding utf8
    if ($job.State -ne 'Completed') { throw "Launcher ended in state $($job.State)." }
} catch {
    $failure = $_.Exception.Message
} finally {
    # A timeout may stop only the synthetic probe we launched, never a pre-existing/manual session.
    if ($failure -and $null -ne $probePid) {
        $active = Get-Process -Id $probePid -ErrorAction SilentlyContinue
        if ($active -and $active.ProcessName -eq 'eden-uwp' -and $active.StartTime -eq $probeStartedAt) {
            $active | Stop-Process -Force
        }
    }
    if ($job.State -eq 'Running') { Stop-Job -Job $job }
    Remove-Job -Job $job -Force
    $pkg = Get-AppxPackage -Name EdenEmuProject.EdenXbox
    if ($pkg) {
        $local = Join-Path $env:LOCALAPPDATA "Packages/$($pkg.PackageFamilyName)/LocalState"
        foreach ($entry in @(@('eden/log/eden_log.txt', 'log.txt'), @('eden_uwp_diag.txt', 'diag.txt'))) {
            $source = Join-Path $local $entry[0]
            if ((Test-Path -LiteralPath $source) -and (Get-Item -LiteralPath $source).LastWriteTimeUtc -ge $started) {
                Copy-Item -LiteralPath $source -Destination (Join-Path $destination $entry[1])
            }
        }
    }
}
$logPath = Join-Path $destination 'log.txt'
$diagPath = Join-Path $destination 'diag.txt'
$log = if (Test-Path $logPath) { Get-Content -LiteralPath $logPath -Raw } else { '' }
$diag = if (Test-Path $diagPath) { Get-Content -LiteralPath $diagPath -Raw } else { '' }
$errors = @($log -split '\r?\n' | Where-Object { $_ -match '<Error>|<Critical>' })
$benches = @($log -split '\r?\n' | Where-Object { $_ -match 'ASTC probe bench:' })
$passed = -not $failure -and $errors.Count -eq 0 -and $log -match 'ASTC probe PASS:' -and
          $log -match 'mismatches=0 ' -and $diag -match 'RunHeadlessBoot returned 0' -and
          $diag -match 'boot worker joined; exiting' -and $benches.Count -eq 32
$gpuValidationActual = $log -match 'D3D12: GPU-based validation on'
if ($GpuValidation -and -not $gpuValidationActual) { $passed = $false; $failure = 'Requested GPU validation was not enabled.' }
if (-not $GpuValidation -and $gpuValidationActual) { $passed = $false; $failure = 'Unexpected GPU validation: timings are not a normal run.' }
if ($GuestIntegration -and $log -notmatch 'ASTC guest validation: .*mismatches=0 ') { $passed = $false; $failure = 'Guest sampler validation did not pass.' }
$summary = [ordered]@{
    passed = [bool]$passed; started_utc = $started.ToString('o'); gpu_validation = [bool]$GpuValidation
    gpu_validation_actual = [bool]$gpuValidationActual
    guest_integration = [bool]$GuestIntegration
    pid = $probePid; process_commit_cap_mib = 5120; process_peak_commit_bytes = $peakCommit
    max_sampled_commit_bytes = $maxSampledCommit; process_peak_working_set_bytes = $peakWorkingSet
    memory_samples = $sampleCount; errors = $errors; failure = $failure
    validation = @($log -split '\r?\n' | Where-Object { $_ -match 'ASTC probe (native )?validation:|ASTC guest validation:' })
    allocations = @($log -split '\r?\n' | Where-Object { $_ -match 'ASTC probe memory:|ASTC probe reference:|ASTC probe fragment target:|ASTC probe observed memory:' })
    compilation = @($log -split '\r?\n' | Where-Object { $_ -match 'ASTC probe compile:' })
    benchmarks = $benches
    timing_scope = $(if ($GpuValidation) { 'Instrumented correctness run; do not use timings for performance.' } else { 'GPU timestamps, debug layer, synthetic corpus, no FPS inference.' })
}
$summary | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $destination 'summary.json') -Encoding utf8
Write-Output "ASTC probe $(if ($passed) { 'PASS' } else { 'FAIL' }): $destination"
if (-not $passed) { throw 'ASTC probe failed. See archived log, diag and summary.json.' }
