# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# The phone gyro: DSU packets, the phone's sample in the pad's frame and the on-screen orientation.
$ErrorActionPreference = 'Stop'
$dsuGate = New-Item -ItemType Directory -Force build-uwp/dsu-phone-gate
$dsuVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$dsuVs) { throw 'MSVC not found' }
$dsuCommands = @(
    '@echo off',
    ('call "' + $dsuVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if not "%errorlevel%"=="0" exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /Zc:preprocessor /W3 /I src tools\xbox\tests\dsu-phone.cpp /Fo:build-uwp\dsu-phone-gate\ /Fe:build-uwp\dsu-phone-gate\dsu-phone.exe',
    'if not "%errorlevel%"=="0" exit /b 1',
    'build-uwp\dsu-phone-gate\dsu-phone.exe',
    'if not "%errorlevel%"=="0" exit /b 1'
)
Set-Content "$($dsuGate.FullName)/build.cmd" $dsuCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($dsuGate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Phone gyro gate failed' }
