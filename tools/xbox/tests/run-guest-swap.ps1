# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Guest swap on a real reservation and swap file: write-out, read back, probation, clears, writers.
$ErrorActionPreference = 'Stop'
$swapVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$swapVs) { throw 'MSVC not found' }
$swapDir = New-Item -ItemType Directory -Force test-results/guest-swap
$swapCommands = @('@echo off', ('call "' + $swapVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /Isrc tools\xbox\tests\guest-swap.cpp /Fotest-results\guest-swap\ /Fetest-results\guest-swap\guest-swap.exe',
    'if errorlevel 1 exit /b 1',
    'test-results\guest-swap\guest-swap.exe',
    'if errorlevel 1 exit /b 1')
Set-Content "$($swapDir.FullName)/run.cmd" $swapCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($swapDir.FullName)/run.cmd" 2>&1 |
    Tee-Object -FilePath "$($swapDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'guest swap gates failed' }
