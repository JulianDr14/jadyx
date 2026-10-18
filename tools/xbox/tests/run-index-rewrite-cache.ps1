# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone gate and microbenchmark of the D3D12 index rewrite cache; no game data needed.
$ErrorActionPreference = 'Stop'
$gateVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$gateVs) { throw 'MSVC not found' }
$gateDir = New-Item -ItemType Directory -Force build-uwp/index-rewrite-cache-gate
$gateCommands = @('@echo off', ('call "' + $gateVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /Zc:preprocessor /EHsc /utf-8 /Isrc tools\xbox\tests\index-rewrite-cache.cpp /Fobuild-uwp\index-rewrite-cache-gate\ /Febuild-uwp\index-rewrite-cache-gate\index-rewrite-cache.exe',
    'if errorlevel 1 exit /b 1', 'build-uwp\index-rewrite-cache-gate\index-rewrite-cache.exe')
Set-Content "$($gateDir.FullName)/run.cmd" $gateCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($gateDir.FullName)/run.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Index rewrite cache gate failed' }
