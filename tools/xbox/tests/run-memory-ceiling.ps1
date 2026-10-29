# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Memory ceilings: the nearest limit, its headroom trend and the log line.
$ErrorActionPreference = 'Stop'
$ceilingVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$ceilingVs) { throw 'MSVC not found' }
$ceilingDir = New-Item -ItemType Directory -Force test-results/memory-ceiling
$ceilingCommands = @('@echo off', ('set "PATH=%PATH%;' + ${env:ProgramFiles(x86)} + '\Microsoft Visual Studio\Installer"'),
    ('call "' + $ceilingVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /Isrc tools\xbox\tests\memory-ceiling.cpp src\common\memory_ceiling.cpp /Fotest-results\memory-ceiling\ /Fetest-results\memory-ceiling\memory-ceiling.exe',
    'if errorlevel 1 exit /b 1',
    'test-results\memory-ceiling\memory-ceiling.exe',
    'if errorlevel 1 exit /b 1')
Set-Content "$($ceilingDir.FullName)/run.cmd" $ceilingCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($ceilingDir.FullName)/run.cmd" 2>&1 |
    Tee-Object -FilePath "$($ceilingDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'memory ceiling gates failed' }
