# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Run from the repository root. Desktop gate for the QR encoder (jadyx/ui/qr_code).
$ErrorActionPreference = 'Stop'
$gate = New-Item -ItemType Directory -Force build-uwp/qr-gate
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC not found' }
$commands = @(
    '@echo off',
    'set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"',
    ('call "' + $vs + '\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul'),
    'if not "%errorlevel%"=="0" exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /W4 /WX /utf-8 /DNOMINMAX /I src tools\xbox\tests\qr-code.cpp src\jadyx\ui\qr_code.cpp /Fo:build-uwp\qr-gate\ /Fe:build-uwp\qr-gate\qr_code.exe',
    'if not "%errorlevel%"=="0" exit /b 1',
    'build-uwp\qr-gate\qr_code.exe',
    'exit /b %errorlevel%'
)
Set-Content "$($gate.FullName)/build.cmd" $commands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($gate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw "QR gate failed (exit $LASTEXITCODE)" }
