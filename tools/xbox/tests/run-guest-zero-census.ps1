# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Guest zero census on a real reservation: zero, written, half-committed, no-access and held granules.
$ErrorActionPreference = 'Stop'
$censusVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$censusVs) { throw 'MSVC not found' }
$censusDir = New-Item -ItemType Directory -Force test-results/guest-zero-census
$censusCommands = @('@echo off', ('set "PATH=%PATH%;' + ${env:ProgramFiles(x86)} + '\Microsoft Visual Studio\Installer"'),
    ('call "' + $censusVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /DNOMINMAX /Isrc tools\xbox\tests\guest-zero-census.cpp /Fotest-results\guest-zero-census\ /Fetest-results\guest-zero-census\guest-zero-census.exe',
    'if errorlevel 1 exit /b 1',
    'test-results\guest-zero-census\guest-zero-census.exe',
    'if errorlevel 1 exit /b 1')
Set-Content "$($censusDir.FullName)/run.cmd" $censusCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($censusDir.FullName)/run.cmd" 2>&1 |
    Tee-Object -FilePath "$($censusDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'guest zero census gates failed' }
