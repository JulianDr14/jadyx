# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# The emulated gyro and the on-screen controller. -Pictures <folder> also saves the drawings.
param([string]$Pictures = '')
$ErrorActionPreference = 'Stop'
$motionGate = New-Item -ItemType Directory -Force build-uwp/motion-emu-gate
$motionVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$motionVs) { throw 'MSVC not found' }
$motionCommands = @(
    '@echo off',
    ('call "' + $motionVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if not "%errorlevel%"=="0" exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /Zc:preprocessor /W3 /DJADYX_NO_WINRT /I src tools\xbox\tests\motion-emu.cpp src\jadyx\game\motion_view.cpp /Fo:build-uwp\motion-emu-gate\ /Fe:build-uwp\motion-emu-gate\motion-emu.exe',
    'if not "%errorlevel%"=="0" exit /b 1',
    ('build-uwp\motion-emu-gate\motion-emu.exe ' + $Pictures),
    'if not "%errorlevel%"=="0" exit /b 1'
)
Set-Content "$($motionGate.FullName)/build.cmd" $motionCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($motionGate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Motion emulation gate failed' }
