# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Host gate for the 5.1 channel mapping (header-only, no build needed).
$ErrorActionPreference = 'Stop'
$testOutput = New-Item -ItemType Directory -Force build-uwp/test-results/surround-layout
$testVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$testVs) { throw 'MSVC not found' }
$testCommands = @('@echo off', ('call "' + $testVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /I src tools\xbox\tests\surround-layout.cpp /Fo:build-uwp\test-results\surround-layout\ /Fe:build-uwp\test-results\surround-layout\surround-layout.exe',
    'if errorlevel 1 exit /b 1')
Set-Content "$($testOutput.FullName)/build.cmd" $testCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($testOutput.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Surround layout gate build failed' }
& "$($testOutput.FullName)/surround-layout.exe"
# Checked directly: a crash's negative exit code slips past "if errorlevel 1".
if ($LASTEXITCODE -ne 0) { throw "Surround layout gate failed ($LASTEXITCODE)" }
