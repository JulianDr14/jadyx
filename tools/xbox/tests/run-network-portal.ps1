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
    $portalZstd = (($portalCache | Where-Object { $_ -match '^CPM_PACKAGE_zstd_SOURCE_DIR:INTERNAL=' }) -replace '^[^=]*=', '') + '/lib'
    $portalArchive = '.cache/cpm/libarchive/v3.8.9/libarchive'
    foreach ($portalDependency in @("$portalHttp/httplib.h", "$portalJson/nlohmann/json.hpp", "$portalSsl/openssl/rand.h", "$portalArchive/archive.h", 'build-uwp/_deps/openssl-build/libcrypto.lib', 'build-uwp/externals/libarchive/archive_read.lib')) {
        if (!(Test-Path -LiteralPath $portalDependency)) { throw "Missing cached dependency: $portalDependency" }
    }
    $portalCommands = @('@echo off', ('call "' + $portalVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if not %errorlevel%==0 exit /b 1')
    $portalCommands += ('cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /Zc:preprocessor /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_WIN32_WINNT=0x0A00 /DLIBARCHIVE_STATIC /I src /I "' + $portalHttp + '" /I "' + $portalJson + '" /I "' + $portalSsl + '" /I "' + $portalZstd + '" /I "' + $portalArchive + '" /I build-uwp/_deps/openssl-build/include tools/xbox/tests/network-portal.cpp src/jadyx/storage/network_portal.cpp src/jadyx/storage/log_export.cpp src/jadyx/storage/nsp_upload.cpp src/jadyx/storage/system_upload.cpp src/jadyx/library/save_archive.cpp /Fo:build-uwp/test-results/network-portal/ /Fe:build-uwp/test-results/network-portal/network-portal.exe /link build-uwp/externals/libarchive/archive_read.lib build-uwp/_deps/xz-build/lzma.lib build-uwp/_deps/zlib-build/zs.lib build-uwp/_deps/zstd-build/lib/zstd_static.lib build-uwp/_deps/openssl-build/libssl.lib build-uwp/_deps/openssl-build/libcrypto.lib ws2_32.lib iphlpapi.lib crypt32.lib bcrypt.lib user32.lib advapi32.lib WindowsApp.lib')
    $portalCommands += 'if not %errorlevel%==0 exit /b 1'
    $portalCommands += 'build-uwp\test-results\network-portal\network-portal.exe'
    $portalCommands += 'if not %errorlevel%==0 exit /b 1'
    Set-Content "$($portalOutput.FullName)/build.cmd" $portalCommands -Encoding ascii
    & "$env:SystemRoot\System32\cmd.exe" /d /c "$($portalOutput.FullName)\build.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Network portal gate failed' }
    $portalLogsFixture = Get-ChildItem -LiteralPath $portalOutput.FullName -Directory -Filter 'logs-*' | Sort-Object CreationTimeUtc -Descending | Select-Object -First 1
    & python tools/xbox/tests/log-export-tar.py $portalLogsFixture.FullName
    if ($LASTEXITCODE -ne 0) { throw 'Log export archive gate failed' }
} finally { Pop-Location }
