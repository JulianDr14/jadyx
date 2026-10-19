# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Native TCP harness, not an AppContainer/Xbox certification. Uses existing cached dependencies.
$ErrorActionPreference = 'Stop'
$portalRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
Push-Location $portalRoot
try {
    $portalOutput = New-Item -ItemType Directory -Force build-uwp/test-results/network-portal
    $portalVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$portalVs) { throw 'MSVC not found' }
    $portalCache = Get-Content build-uwp/CMakeCache.txt
    $portalHttp = ($portalCache | Where-Object { $_ -match '^httplib_SOURCE_DIR:STATIC=' }) -replace '^[^=]*=', ''
    $portalJson = (($portalCache | Where-Object { $_ -match '^nlohmann_json_SOURCE_DIR:STATIC=' }) -replace '^[^=]*=', '') + '/include'
    $portalSsl = (($portalCache | Where-Object { $_ -match '^CPM_PACKAGE_OpenSSL_SOURCE_DIR:INTERNAL=' }) -replace '^[^=]*=', '') + '/include'
    foreach ($portalDependency in @("$portalHttp/httplib.h", "$portalJson/nlohmann/json.hpp", "$portalSsl/openssl/rand.h", 'build-uwp/_deps/openssl-build/libcrypto.lib')) {
        if (!(Test-Path -LiteralPath $portalDependency)) { throw "Missing cached dependency: $portalDependency" }
    }
    $portalCommands = @('@echo off', ('call "' + $portalVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if errorlevel 1 exit /b 1')
    $portalCommands += ('cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /Zc:preprocessor /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /I src /I "' + $portalHttp + '" /I "' + $portalJson + '" /I "' + $portalSsl + '" /I build-uwp/_deps/openssl-build/include tools/xbox/tests/network-portal.cpp src/jadyx/storage/network_portal.cpp src/jadyx/storage/nsp_upload.cpp /Fo:build-uwp/test-results/network-portal/ /Fe:build-uwp/test-results/network-portal/network-portal.exe /link build-uwp/_deps/openssl-build/libssl.lib build-uwp/_deps/openssl-build/libcrypto.lib ws2_32.lib iphlpapi.lib crypt32.lib bcrypt.lib user32.lib advapi32.lib WindowsApp.lib')
    $portalCommands += 'if errorlevel 1 exit /b 1'
    $portalCommands += 'build-uwp\test-results\network-portal\network-portal.exe'
    $portalCommands += 'if errorlevel 1 exit /b 1'
    Set-Content "$($portalOutput.FullName)/build.cmd" $portalCommands -Encoding ascii
    & "$env:SystemRoot\System32\cmd.exe" /d /c "$($portalOutput.FullName)\build.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Network portal gate failed' }
} finally { Pop-Location }
