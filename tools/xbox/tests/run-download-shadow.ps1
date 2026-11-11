# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Download merge gate, desktop only, without game data. Runs with and without AVX2.
$ErrorActionPreference = 'Stop'
$shadowVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$shadowVs) { throw 'MSVC not found' }
$shadowDir = New-Item -ItemType Directory -Force test-results/download-shadow
$shadowCommands = @('@echo off',
    'set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"', ('call "' + $shadowVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W3 /Isrc tools\xbox\tests\download-shadow.cpp /Fotest-results\download-shadow\ /Fetest-results\download-shadow\scalar.exe',
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W3 /arch:AVX2 /Isrc tools\xbox\tests\download-shadow.cpp /Fotest-results\download-shadow\ /Fetest-results\download-shadow\avx2.exe',
    'if errorlevel 1 exit /b 1',
    'test-results\download-shadow\scalar.exe',
    'if not "%errorlevel%"=="0" exit /b 1',
    'test-results\download-shadow\avx2.exe',
    'if not "%errorlevel%"=="0" exit /b 1')
Set-Content "$($shadowDir.FullName)/run.cmd" $shadowCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($shadowDir.FullName)/run.cmd" 2>&1 |
    Tee-Object -FilePath "$($shadowDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'download shadow gate failed' }
