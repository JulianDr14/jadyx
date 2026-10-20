# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Registers the prepared diagnostic layout, keeping the signed Series APPX unchanged.
# Run with Windows PowerShell 5.1, after closing any running PC gameplay yourself.
[CmdletBinding()]
param([switch]$NoLaunch)
$ErrorActionPreference = 'Stop'
if ($PSVersionTable.PSEdition -ne 'Desktop') { throw 'Use powershell.exe (Windows PowerShell 5.1) for Appx registration.' }
$captureRoot = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$captureLayout = Join-Path $captureRoot 'build-uwp/package/layout'
$captureAppx = Join-Path $captureRoot 'build-uwp/package/jadyx.appx'
if (Get-Process -Name jadyx -ErrorAction SilentlyContinue) { throw 'Close PC gameplay with Q or the game menu before starting this capture session.' }
$captureConfigPath = Join-Path $captureLayout 'boot.cfg'
$captureConfig = [IO.File]::ReadAllLines($captureConfigPath)
if ($captureConfig -notcontains 'gpu_capture_full=1' -or $captureConfig -notcontains 'library=1') {
    throw 'Prepare the full-capture library package first; the current layout is not that diagnostic build.'
}
Add-Type -AssemblyName System.IO.Compression.FileSystem
$captureZip = [IO.Compression.ZipFile]::OpenRead($captureAppx)
try {
    $captureEntry = $captureZip.GetEntry('jadyx.exe')
    if (!$captureEntry) { throw 'The signed APPX lacks jadyx.exe' }
    $captureStream = $captureEntry.Open()
    $captureSha = [Security.Cryptography.SHA256]::Create()
    try { $captureExeHash = [BitConverter]::ToString($captureSha.ComputeHash($captureStream)).Replace('-','') }
    finally { $captureSha.Dispose(); $captureStream.Dispose() }
} finally { $captureZip.Dispose() }
if ((Get-FileHash -LiteralPath (Join-Path $captureLayout 'jadyx.exe') -Algorithm SHA256).Hash -ne $captureExeHash) {
    throw 'PC layout executable does not match the Series APPX.'
}
$capturePackageHash = (Get-FileHash -LiteralPath $captureAppx -Algorithm SHA256).Hash
# Only the unpacked PC configuration changes. No repack, no payload and no user-data reset.
$captureConfig = @($captureConfig | Where-Object { $_ -notmatch '^memory_limit_mib=' }) + 'memory_limit_mib=5120'
[IO.File]::WriteAllLines($captureConfigPath, $captureConfig, [Text.UTF8Encoding]::new($false))
& icacls $captureLayout /grant '*S-1-15-2-1:(OI)(CI)RX' /T /Q | Out-Null
if ($LASTEXITCODE -ne 0) { throw 'Could not grant the AppContainer read access to the layout.' }
Add-AppxPackage -Register (Join-Path $captureLayout 'AppxManifest.xml')
if ((Get-FileHash -LiteralPath $captureAppx -Algorithm SHA256).Hash -ne $capturePackageHash) {
    throw 'The Series APPX changed unexpectedly.'
}
$capturePackage = Get-AppxPackage -Name Jadyx
Write-Host "PC $($capturePackage.Version), cap 5120 MiB; same executable as Series; APPX preserved."
if (!$NoLaunch) { Start-Process ('shell:AppsFolder\' + $capturePackage.PackageFamilyName + '!App') }
