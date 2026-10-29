# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Driver memory reserve: commit, release, commit again and off, on the process's private commit.
$ErrorActionPreference = 'Stop'
$reserveVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$reserveVs) { throw 'MSVC not found' }
$reserveDir = New-Item -ItemType Directory -Force test-results/memory-reserve
# The reserve logs through common/logging.h; the gate prints nothing of it.
$reserveStub = New-Item -ItemType Directory -Force "$($reserveDir.FullName)/stub/common"
Set-Content "$($reserveStub.FullName)/logging.h" @('#pragma once',
    '#define LOG_INFO(...) ((void)0)', '#define LOG_WARNING(...) ((void)0)') -Encoding ascii
$reserveCommands = @('@echo off', ('call "' + $reserveVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /Itest-results\memory-reserve\stub /Isrc tools\xbox\tests\memory-reserve.cpp /Fotest-results\memory-reserve\ /Fetest-results\memory-reserve\memory-reserve.exe',
    'if errorlevel 1 exit /b 1',
    'test-results\memory-reserve\memory-reserve.exe',
    'if errorlevel 1 exit /b 1')
Set-Content "$($reserveDir.FullName)/run.cmd" $reserveCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($reserveDir.FullName)/run.cmd" 2>&1 |
    Tee-Object -FilePath "$($reserveDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'memory reserve gates failed' }
