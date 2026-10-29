# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Permafrost on a real reservation and swap file: write-out, windowed read back, probation, probes,
# refault shields, clears, writers.
$ErrorActionPreference = 'Stop'
$swapVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$swapVs) { throw 'MSVC not found' }
$swapDir = New-Item -ItemType Directory -Force test-results/permafrost
$swapCommands = @('@echo off', ('call "' + $swapVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /Isrc tools\xbox\tests\permafrost.cpp /Fotest-results\permafrost\ /Fetest-results\permafrost\permafrost.exe',
    'if errorlevel 1 exit /b 1',
    'test-results\permafrost\permafrost.exe',
    'if errorlevel 1 exit /b 1')
Set-Content "$($swapDir.FullName)/run.cmd" $swapCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($swapDir.FullName)/run.cmd" 2>&1 |
    Tee-Object -FilePath "$($swapDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'permafrost gates failed' }
