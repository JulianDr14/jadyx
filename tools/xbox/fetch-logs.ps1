# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Downloads Jadyx's logs (LocalState\jadyx\log) from the console through the Device Portal REST
# API into a folder of their own. The installed package is looked up, so any version works.
# The portal's certificate is self-signed: curl.exe runs with -k.
param(
    [string] $Console = $(if ($env:JADYX_CONSOLE) { $env:JADYX_CONSOLE } else { '192.168.1.50' }),
    [string] $OutDir = ''
)
$ErrorActionPreference = 'Stop'
$portal = "https://${Console}:11443"

function Get-Portal([string] $path, [string] $outFile = '') {
    $curlArgs = @('-sSk', '--fail', '--max-time', '120', "$portal$path")
    if ($outFile) { $curlArgs += @('-o', $outFile) }
    $text = & curl.exe @curlArgs
    if ($LASTEXITCODE -ne 0) { throw "Device Portal request failed ($LASTEXITCODE): $path" }
    return $text
}

$packages = (Get-Portal '/api/app/packagemanager/packages' | Out-String | ConvertFrom-Json).InstalledPackages
$package = $packages | Where-Object { $_.PackageFullName -like 'Jadyx_*' } | Select-Object -First 1
if (!$package) { throw "Jadyx is not installed on $Console" }
$name = $package.PackageFullName
$query = "knownfolderid=LocalAppData&packagefullname=$name&path=" + [uri]::EscapeDataString('\LocalState\jadyx\log')
$items = (Get-Portal "/api/filesystem/apps/files?$query" | Out-String | ConvertFrom-Json).Items |
    Where-Object { $_.Type -ne 16 }

if (!$OutDir) {
    $OutDir = Join-Path $PSScriptRoot "..\..\build-uwp\console-logs\$(Get-Date -Format 'yyyy-MM-dd_HH-mm-ss')"
}
$folder = New-Item -ItemType Directory -Force $OutDir
foreach ($item in $items) {
    Get-Portal "/api/filesystem/apps/file?$query&filename=$([uri]::EscapeDataString($item.Name))" `
        (Join-Path $folder.FullName $item.Name) | Out-Null
}
Write-Host "$name -> $($folder.FullName)"
Get-ChildItem $folder | Select-Object Name, Length | Format-Table -AutoSize | Out-String | Write-Host
