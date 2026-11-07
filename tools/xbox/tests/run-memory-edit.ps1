# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Jadyx Memory Edit without the game: typed numbers, AVX2 against plain comparison, result chunks, and
# a search over a real Permafrost reservation and swap file. Built with AVX2, as the console is.
$ErrorActionPreference = 'Stop'
$jmeVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$jmeVs) { throw 'MSVC not found' }
$jmeDir = New-Item -ItemType Directory -Force test-results/memory-edit
$jmeCommands = @('@echo off', ('call "' + $jmeVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /arch:AVX2 /std:c++20 /EHsc /utf-8 /W4 /WX /Isrc tools\xbox\tests\memory_edit.cpp /Fotest-results\memory-edit\ /Fetest-results\memory-edit\memory-edit.exe',
    'if errorlevel 1 exit /b 1',
    'test-results\memory-edit\memory-edit.exe',
    # A failed assert ends the process with a negative code, which "if errorlevel 1" lets through.
    'if %errorlevel% neq 0 exit /b 1')
Set-Content "$($jmeDir.FullName)/run.cmd" $jmeCommands -Encoding ascii
# Joined inside cmd: PowerShell 5.1 with 'Stop' dies on any native stderr line, and vcvars writes some.
& "$env:SystemRoot/System32/cmd.exe" /d /c "`"$($jmeDir.FullName)\run.cmd`" 2>&1" |
    Tee-Object -FilePath "$($jmeDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'memory edit gates failed' }
