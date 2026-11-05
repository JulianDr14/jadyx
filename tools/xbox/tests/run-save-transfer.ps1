# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# The portal's save archives (save-transfer.cpp), natively. Uses the libraries build-uwp already built.
$ErrorActionPreference = 'Stop'
$saveRoot = (Resolve-Path (Join-Path $PSScriptRoot '../../..')).Path
Push-Location $saveRoot
try {
    $saveOutput = New-Item -ItemType Directory -Force build-uwp/test-results/save-transfer
    $saveVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$saveVs) { throw 'MSVC not found' }
    $saveCache = Get-Content build-uwp/CMakeCache.txt
    $saveArchive = (($saveCache | Where-Object { $_ -match '^CPM_PACKAGE_libarchive_SOURCE_DIR:INTERNAL=' }) -replace '^[^=]*=', '') + '/libarchive'
    $saveLibraries = @('build-uwp/externals/libarchive/archive_read.lib', 'build-uwp/_deps/zlib-build/zs.lib', 'build-uwp/_deps/zstd-build/lib/zstd_static.lib', 'build-uwp/_deps/xz-build/lzma.lib')
    foreach ($saveDependency in @("$saveArchive/archive.h") + $saveLibraries) {
        if (!(Test-Path -LiteralPath $saveDependency)) { throw "Missing cached dependency: $saveDependency" }
    }
    $saveCommands = @('@echo off', ('call "' + $saveVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if not %errorlevel%==0 exit /b 1')
    $saveCommands += ('cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W3 /DNOMINMAX /DLIBARCHIVE_STATIC /I src /I tools/xbox/tests /I "' + $saveArchive + '" tools/xbox/tests/save-transfer.cpp src/jadyx/library/save_transfer.cpp src/jadyx/mods/mod_archive.cpp src/jadyx/mods/mod_layout.cpp /Fo:build-uwp/test-results/save-transfer/ /Fe:build-uwp/test-results/save-transfer/save-transfer.exe /link ' + ($saveLibraries -join ' ') + ' bcrypt.lib advapi32.lib')
    $saveCommands += 'if not %errorlevel%==0 exit /b 1'
    $saveCommands += 'build-uwp\test-results\save-transfer\save-transfer.exe'
    $saveCommands += 'if not %errorlevel%==0 exit /b 1'
    Set-Content "$($saveOutput.FullName)/build.cmd" $saveCommands -Encoding ascii
    & "$env:SystemRoot\System32\cmd.exe" /d /c "$($saveOutput.FullName)\build.cmd"
    if ($LASTEXITCODE -ne 0) { throw 'Save transfer gate failed' }
} finally { Pop-Location }
