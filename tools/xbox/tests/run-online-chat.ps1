# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Run from the repository root. Desktop gate for the room chat's history.
$ErrorActionPreference = 'Stop'
$chatGate = New-Item -ItemType Directory -Force build-uwp/online-chat-gate
$chatVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$chatVs) { throw 'MSVC not found' }
$chatCommands = @(
    '@echo off',
    ('call "' + $chatVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if not "%errorlevel%"=="0" exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /W4 /WX /utf-8 /I src tools\xbox\tests\online-chat.cpp /Fo:build-uwp\online-chat-gate\ /Fe:build-uwp\online-chat-gate\online-chat.exe',
    'if not "%errorlevel%"=="0" exit /b 1',
    'build-uwp\online-chat-gate\online-chat.exe',
    'exit /b %errorlevel%'
)
Set-Content "$($chatGate.FullName)/build.cmd" $chatCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($chatGate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Online chat gate failed' }
