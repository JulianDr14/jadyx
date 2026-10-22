# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Buffer cache async download ordering gate, desktop only, without game data.
$ErrorActionPreference = 'Stop'
$supersedeVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$supersedeVs) { throw 'MSVC not found' }
$supersedeBoost = Join-Path (Get-Location) '.cache/cpm/boost/1.92.0/libs'
if (!(Test-Path $supersedeBoost)) { throw "Boost sources not found at $supersedeBoost (configure the build once)" }
# Modular Boost: every library keeps its headers under libs/<name>/include.
$supersedeIncludes = (Get-ChildItem $supersedeBoost -Directory |
    Where-Object { Test-Path "$($_.FullName)/include" } |
    ForEach-Object { '/I"' + $_.FullName + '\include"' }) -join ' '
$supersedeDir = New-Item -ItemType Directory -Force test-results/async-download-supersede
$supersedeCommands = @('@echo off',
    'set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"', ('call "' +$supersedeVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    "cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W3 /Isrc $supersedeIncludes tools\xbox\tests\async-download-supersede.cpp /Fotest-results\async-download-supersede\ /Fetest-results\async-download-supersede\async-download-supersede.exe",
    'if errorlevel 1 exit /b 1',
    'test-results\async-download-supersede\async-download-supersede.exe',
    'if errorlevel 1 exit /b 1')
Set-Content "$($supersedeDir.FullName)/run.cmd" $supersedeCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($supersedeDir.FullName)/run.cmd" 2>&1 |
    Tee-Object -FilePath "$($supersedeDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'async download supersede gate failed' }
