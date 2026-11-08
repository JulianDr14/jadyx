# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds jadyx, packages it into build-uwp\package and installs it, on the console or on this PC.
#
#   powershell -ExecutionPolicy Bypass -File tools\xbox\deploy.ps1 -Version 0.3.8.31
#       [-Target Console|Pc] [-BootCfg "key=value","key=value"] [-NoBuild] [-NoPackage]
#       [-Console 192.168.1.50] [-BuildDir build-uwp-nolto]
#
# -BuildDir picks the binary dir to build and package: build-uwp (LTO, for performance work and
# releases) or build-uwp-nolto (preset uwp-x64-nolto, fast links for fixes that need many builds).
# The package always lands in build-uwp\package.
#
# Console: uploads the package, its VCLibs dependency and the certificate through the Device
# Portal REST API and waits for the install to finish. The app keeps its Game mode setting.
# -NoPackage installs what build-uwp\package already holds (a payload package from package-appx.ps1,
# for one); -Version must then be the version it was packed with.
# Pc: hands over to local-run.ps1 (loose register in Developer Mode, launch, wait for the boot).
param(
    [Parameter(Mandatory)] [string] $Version,
    [ValidateSet('Console', 'Pc')] [string] $Target = 'Console',
    [string[]] $BootCfg = @(),
    [switch] $NoBuild,
    [switch] $NoPackage,
    [ValidateSet('build-uwp', 'build-uwp-nolto')] [string] $BuildDir = 'build-uwp',
    [string] $Console = $(if ($env:JADYX_CONSOLE) { $env:JADYX_CONSOLE } else { '192.168.1.50' })
)
$ErrorActionPreference = 'Stop'
$r = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path

if ($Target -eq 'Pc') {
    & "$PSScriptRoot\local-run.ps1" -Library -BootCfg $BootCfg -NoBuild:$NoBuild
    return
}

if (-not $NoBuild) {
    # cmd redirects its own output: under 'Stop', PowerShell 5.1 turns any stderr line (a CMake
    # deprecation warning on a reconfigure) into a terminating error.
    & $env:ComSpec /c "call `"$r\tools\xbox\build-env.bat`" && cmake --build `"$r\$BuildDir`" --config Release --target jadyx > `"$r\$BuildDir\build.log`" 2>&1"
    if ($LASTEXITCODE -ne 0) {
        Select-String -Path "$r\$BuildDir\build.log" -Pattern ' error ' | Select-Object -First 10 | Write-Host
        throw "build failed ($BuildDir\build.log)"
    }
}
if (-not $NoPackage) {
    Push-Location $r
    try {
        & "$r\tools\xbox\package-appx.ps1" -BuildDir $BuildDir -Library -Version $Version -BootCfg $BootCfg *> "$r\build-uwp\package.log"
    } finally {
        Pop-Location
    }
}
$package = "$r\build-uwp\package"
foreach ($file in 'jadyx.appx', 'Microsoft.VCLibs.x64.14.00.appx', 'jadyx.cer') {
    if (-not (Test-Path "$package\$file")) { throw "missing $file (build-uwp\package.log)" }
}

# The portal answers POSTs only with the CSRF token it sets as a cookie on any GET. Its
# certificate is self-signed: curl.exe runs with -k.
$portal = "https://${Console}:11443"
$jar = Join-Path ([IO.Path]::GetTempPath()) "jadyx-portal-$PID.txt"
try {
    & curl.exe -sSk --fail --max-time 30 -c $jar "$portal/api/os/info" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Device Portal not reachable at $portal" }
    $token = (Get-Content $jar | Where-Object { $_ -match "`tCSRF-Token`t" } | Select-Object -First 1) -split "`t" |
        Select-Object -Last 1
    if (-not $token) { throw "the portal sent no CSRF token" }
    Write-Host "uploading $Version to $Console..."
    & curl.exe -sSk --fail --max-time 600 -b $jar -H "X-CSRF-Token: $token" `
        -F "jadyx.appx=@$package\jadyx.appx" `
        -F "Microsoft.VCLibs.x64.14.00.appx=@$package\Microsoft.VCLibs.x64.14.00.appx" `
        -F "jadyx.cer=@$package\jadyx.cer" `
        "$portal/api/app/packagemanager/package?package=jadyx.appx" | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "upload failed ($LASTEXITCODE)" }
    # 204 while the install runs, 200 with the result once it is done.
    $deadline = (Get-Date).AddMinutes(5)
    do {
        Start-Sleep -Seconds 2
        $state = & curl.exe -sSk --max-time 30 -b $jar -w "`n%{http_code}" "$portal/api/app/packagemanager/state"
        $code = ($state | Select-Object -Last 1)
    } while ($code -eq '204' -and (Get-Date) -lt $deadline)
    if ($code -ne '200') { throw "install did not finish (HTTP $code): $($state -join ' ')" }
    $result = ($state | Select-Object -SkipLast 1) -join ' '
    # A finished install can still be a rejected one (a higher version installed, for one).
    if (-not ($result | ConvertFrom-Json).Success) { throw "install of $Version failed: $result" }
    Write-Host "installed $Version on ${Console}: $result"
} finally {
    Remove-Item -LiteralPath $jar -ErrorAction SilentlyContinue
}
