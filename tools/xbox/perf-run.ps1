# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One unattended performance run on the console: packages the built jadyx, installs it, optionally
# puts back a game's save folder, launches a game from the library with a scripted controller,
# waits for quit_at, downloads the logs and prints the perf summary (perf-summary.py).
#
#   powershell -ExecutionPolicy Bypass -File tools\xbox\perf-run.ps1 -Version 0.3.9.35
#       -Game "jadyx-usb:/.../game.nsp" -Seconds 300 [-Pad "20:A","25:A"] [-BootCfg "gpu_profile=1"]
#       [-SaveBackup <local folder> -SavePath "\LocalState\jadyx\nand\user\save\...\<title>"]
#       [-OutDir <folder>] [-NoInstall]
#
# The run never uninstalls: -Version must be above the installed one. Every portal call retries,
# so a dropped connection costs a retry, not the run.
param(
    [Parameter(Mandatory)] [string] $Version,
    [Parameter(Mandatory)] [string] $Game,
    [int] $Seconds = 300,
    [string[]] $Pad = @(),
    [string[]] $BootCfg = @('async_record=1', 'gpu_profile=1'),
    [string] $SaveBackup = '',
    [string] $SavePath = '',
    [string] $OutDir = '',
    [switch] $NoInstall,
    [string] $Console = $(if ($env:JADYX_CONSOLE) { $env:JADYX_CONSOLE } else { '192.168.1.50' })
)
$ErrorActionPreference = 'Stop'
$r = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$portal = "https://${Console}:11443"
$jar = Join-Path ([IO.Path]::GetTempPath()) "jadyx-perf-$PID.txt"
if (!$OutDir) { $OutDir = Join-Path $r "build-uwp\perf-runs\$Version-$(Get-Date -Format 'MMdd-HHmm')" }
New-Item -ItemType Directory -Force $OutDir | Out-Null

function Invoke-Portal([string[]] $curlArgs, [int] $tries = 6) {
    for ($i = 1; ; ++$i) {
        $out = & curl.exe -sSk --max-time 120 -b $jar -c $jar @curlArgs
        if ($LASTEXITCODE -eq 0) { return $out }
        if ($i -ge $tries) { throw "portal request failed ($LASTEXITCODE): $($curlArgs -join ' ')" }
        Start-Sleep -Seconds (10 * $i)
    }
}
function Get-Token {
    Invoke-Portal @("$portal/api/os/info") | Out-Null
    return (Get-Content $jar | Where-Object { $_ -match "`tCSRF-Token`t" } | Select-Object -First 1) -split "`t" |
        Select-Object -Last 1
}
function Get-Package {
    $list = (Invoke-Portal @("$portal/api/app/packagemanager/packages") | Out-String | ConvertFrom-Json).InstalledPackages
    return $list | Where-Object { $_.PackageFullName -like 'Jadyx_*' } |
        Sort-Object { [version]"$($_.Version.Major).$($_.Version.Minor).$($_.Version.Build).$($_.Version.Revision)" } -Descending |
        Select-Object -First 1
}
function B64([string] $s) { [uri]::EscapeDataString([Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($s))) }

try {
    $cfg = @('play=1', "library_pick=$Game", "quit_at=$Seconds") + $BootCfg + ($Pad | ForEach-Object { "input=$_" })
    if (!$NoInstall) {
        # deploy.ps1 retries nothing itself: a failed upload is tried again here.
        for ($i = 1; ; ++$i) {
            try {
                & "$PSScriptRoot\deploy.ps1" -Version $Version -NoBuild -BootCfg ($cfg -join ',') -Console $Console
                break
            } catch {
                if ($i -ge 4) { throw }
                Write-Host "install attempt $i failed: $_"
                Start-Sleep -Seconds 60
            }
        }
    }
    $token = Get-Token
    $package = Get-Package
    if ($package.PackageFullName -notlike "Jadyx_$($Version)_*") { throw "installed package is $($package.PackageFullName), not $Version" }
    $name = $package.PackageFullName
    $pkgArg = B64 $name

    # A running copy (the last run's library) is closed first.
    Invoke-Portal @('-H', "X-CSRF-Token: $token", '-X', 'DELETE', "$portal/api/taskmanager/app?package=$pkgArg") | Out-Null
    Start-Sleep -Seconds 3

    # The logs of the previous run go away, so every file downloaded belongs to this one.
    $logQuery = "knownfolderid=LocalAppData&packagefullname=$name&path=" + [uri]::EscapeDataString('\LocalState\jadyx') + "&filename=log"
    & curl.exe -sSk --max-time 60 -b $jar -H "X-CSRF-Token: $token" -X DELETE "$portal/api/filesystem/apps/folder?$logQuery" | Out-Null

    if ($SaveBackup) {
        foreach ($file in Get-ChildItem -Recurse -File $SaveBackup) {
            $rel = $file.DirectoryName.Substring((Resolve-Path $SaveBackup).Path.Length).TrimStart('\')
            $dir = if ($rel) { "$SavePath\$rel" } else { $SavePath }
            $q = "knownfolderid=LocalAppData&packagefullname=$name&path=" + [uri]::EscapeDataString($dir)
            Invoke-Portal @('-H', "X-CSRF-Token: $token", '-F', "file=@$($file.FullName)", "$portal/api/filesystem/apps/file?$q") | Out-Null
        }
        Write-Host "save folder restored from $SaveBackup"
    }

    $appArg = B64 $package.PackageRelativeId
    Invoke-Portal @('-H', "X-CSRF-Token: $token", '-X', 'POST', '-H', 'Content-Length: 0', "$portal/api/taskmanager/app?appid=$appArg&package=$pkgArg") | Out-Null
    $started = Get-Date
    Write-Host "launched $name at $started; waiting for quit_at=$Seconds"

    # Boot (shader cache, JIT prewarm) plus the session plus the shutdown.
    $diagQuery = "knownfolderid=LocalAppData&packagefullname=$name&path=" + [uri]::EscapeDataString('\LocalState\jadyx\log')
    $deadline = $started.AddSeconds($Seconds + 420)
    $finished = $false
    Start-Sleep -Seconds ([Math]::Min($Seconds, 120))
    while ((Get-Date) -lt $deadline) {
        Start-Sleep -Seconds 20
        $diag = & curl.exe -sSk --max-time 60 "$portal/api/filesystem/apps/file?$diagQuery&filename=jadyx_diag.txt"
        if ($LASTEXITCODE -ne 0) { continue }
        if ($diag -match 'quit_at: shutting down|library: back from the game|step: the guest exited|GPU thread reported a failure|CRASH:|abort stack:') {
            $finished = $true
            Start-Sleep -Seconds 15
            break
        }
    }
    if (!$finished) { Write-Host "the run did not report its end before the deadline" }
    $token = Get-Token
    Invoke-Portal @('-H', "X-CSRF-Token: $token", '-X', 'DELETE', "$portal/api/taskmanager/app?package=$pkgArg") | Out-Null
    Start-Sleep -Seconds 5

    for ($i = 1; ; ++$i) {
        try { & "$PSScriptRoot\fetch-logs.ps1" -Console $Console -OutDir $OutDir | Out-Null; break }
        catch { if ($i -ge 6) { throw }; Start-Sleep -Seconds (15 * $i) }
    }
    $cfg | Set-Content -Encoding utf8 (Join-Path $OutDir 'boot.cfg.txt')
    & python "$PSScriptRoot\perf-summary.py" $OutDir | Tee-Object -FilePath (Join-Path $OutDir 'summary.txt')
} finally {
    Remove-Item -LiteralPath $jar -ErrorAction SilentlyContinue
}
