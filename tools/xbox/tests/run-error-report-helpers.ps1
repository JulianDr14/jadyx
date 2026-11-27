# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Error-report helper gate, desktop only, without game data.
$ErrorActionPreference = 'Stop'
$helpersVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$helpersVs) { throw 'MSVC not found' }
$helpersDir = New-Item -ItemType Directory -Force test-results/error-report-helpers
$helpersCommands = @('@echo off',
    'set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"', ('call "' + $helpersVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /Isrc tools\xbox\tests\error-report-helpers.cpp /Fotest-results\error-report-helpers\ /Fetest-results\error-report-helpers\helpers.exe',
    'if errorlevel 1 exit /b 1',
    'test-results\error-report-helpers\helpers.exe',
    'if not "%errorlevel%"=="0" exit /b 1')
Set-Content "$($helpersDir.FullName)/run.cmd" $helpersCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($helpersDir.FullName)/run.cmd" 2>&1 |
    Tee-Object -FilePath "$($helpersDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'error-report helpers gate failed' }
