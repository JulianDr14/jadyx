# SPDX-FileCopyrightText: Copyright 2026 JulianDr14
# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone gates for CB_DATA runs, blit destination discards and log rotation; no game data.
$ErrorActionPreference = 'Stop'
$gateVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$gateVs) { throw 'MSVC not found' }
$gateDir = New-Item -ItemType Directory -Force build-uwp/transfer-gates
$gateCommands = @('@echo off', ('call "' + $gateVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1')
foreach ($gate in 'const-buffer-run', 'blit-coverage', 'log-rotation') {
    $gateCommands += "cl /nologo /O2 /MD /std:c++20 /Zc:preprocessor /EHsc /utf-8 /W4 /WX /Isrc tools\xbox\tests\$gate.cpp /Fobuild-uwp\transfer-gates\ /Febuild-uwp\transfer-gates\$gate.exe"
    $gateCommands += 'if errorlevel 1 exit /b 1'
    $gateCommands += "build-uwp\transfer-gates\$gate.exe"
    $gateCommands += 'if errorlevel 1 exit /b 1'
}
Set-Content "$($gateDir.FullName)/run.cmd" $gateCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($gateDir.FullName)/run.cmd"
if ($LASTEXITCODE -ne 0) { throw 'transfer gates failed' }
