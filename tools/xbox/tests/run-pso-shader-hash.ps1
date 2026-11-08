# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$hashVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$hashVs) { throw 'MSVC not found' }
$hashDir = New-Item -ItemType Directory -Force build-uwp/pso-shader-hash-gate
$hashCommands = @('@echo off', ('call "' + $hashVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /W4 /WX /MD /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /Isrc tools\xbox\tests\pso-shader-hash.cpp /Fobuild-uwp\pso-shader-hash-gate\ /Febuild-uwp\pso-shader-hash-gate\pso-shader-hash.exe',
    'if errorlevel 1 exit /b 1', 'build-uwp\pso-shader-hash-gate\pso-shader-hash.exe')
Set-Content "$($hashDir.FullName)/run.cmd" $hashCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($hashDir.FullName)/run.cmd"
if ($LASTEXITCODE -ne 0) { throw 'PSO shader hash gate failed' }
