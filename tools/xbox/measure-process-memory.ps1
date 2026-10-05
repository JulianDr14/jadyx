# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
param([Parameter(Mandatory=$true)][int]$TargetProcessId,
      [Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$initial=Get-Process -Id $TargetProcessId
if ($initial.ProcessName -ne 'eden-uwp') { throw 'Expected eden-uwp.' }
$startTime=$initial.StartTime
$samples=Join-Path $OutputDirectory 'process-memory.csv'
$writer=New-Object IO.StreamWriter($samples,$false,(New-Object Text.UTF8Encoding($false)))
$writer.WriteLine('utc,commit_bytes,working_set_bytes,peak_commit_bytes,peak_working_set_bytes,cpu_seconds')
$peakCommit=0L; $sampled=0L; $peakWorkingSet=0L; $count=0L
function Publish-MemorySummary($value) {
    $summaryPath=Join-Path $OutputDirectory 'memory-summary.json'
    $temporary=$summaryPath+'.tmp'
    [IO.File]::WriteAllText($temporary,($value | ConvertTo-Json),(New-Object Text.UTF8Encoding($false)))
    try {
        if ([IO.File]::Exists($summaryPath)) { [IO.File]::Replace($temporary,$summaryPath,[NullString]::Value) }
        else { [IO.File]::Move($temporary,$summaryPath) }
    } catch [IO.IOException] {
        # A reader can briefly deny replacement on Windows. Keep sampling; retry next tick.
    }
}
try {
    while ($true) {
        $current=Get-Process -Id $TargetProcessId -ErrorAction SilentlyContinue
        if (-not $current -or $current.StartTime -ne $startTime) { break }
        $peakCommit=[Math]::Max($peakCommit,$current.PeakPagedMemorySize64)
        $sampled=[Math]::Max($sampled,$current.PagedMemorySize64)
        $peakWorkingSet=[Math]::Max($peakWorkingSet,$current.PeakWorkingSet64)
        $writer.WriteLine(('{0},{1},{2},{3},{4},{5}' -f [DateTime]::UtcNow.ToString('o'),$current.PagedMemorySize64,$current.WorkingSet64,$current.PeakPagedMemorySize64,$current.PeakWorkingSet64,$current.TotalProcessorTime.TotalSeconds.ToString([Globalization.CultureInfo]::InvariantCulture)))
        $writer.Flush(); $count++
        $summary=[ordered]@{pid=$TargetProcessId;running=$true;process_start=$startTime.ToString('o');samples=$count;peak_commit_bytes=$peakCommit;max_sampled_commit_bytes=$sampled;peak_working_set_bytes=$peakWorkingSet;scope='PC process commit; dedicated GPU memory is separate. ASTC allocation accounting is in the renderer log.'}
        Publish-MemorySummary $summary
        Start-Sleep -Milliseconds 500
    }
} finally { $writer.Dispose() }
$summary.running=$false
Publish-MemorySummary $summary
$local=Join-Path $env:LOCALAPPDATA 'Packages/EdenEmuProject.EdenXbox_4qge6yz81zw0w/LocalState'
Copy-Item -LiteralPath (Join-Path $local 'eden/log/eden_log.txt') -Destination (Join-Path $OutputDirectory 'log.txt') -ErrorAction SilentlyContinue
Copy-Item -LiteralPath (Join-Path $local 'eden_uwp_diag.txt') -Destination (Join-Path $OutputDirectory 'diag.txt') -ErrorAction SilentlyContinue
