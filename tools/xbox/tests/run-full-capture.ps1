# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$captureGate = New-Item -ItemType Directory -Force 'build-uwp/full-capture-gate'
$captureVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$captureVs) { throw 'MSVC not found' }
$captureCommands = @('@echo off', ('call "' + $captureVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /EHsc /I src tools\xbox\tests\full-capture.cpp src\video_core\diagnostics\full_capture.cpp /Fo:build-uwp\full-capture-gate\ /Fe:build-uwp\full-capture-gate\full-capture.exe')
Set-Content "$($captureGate.FullName)/build.cmd" $captureCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($captureGate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Full capture gate build failed' }
$captureOutput = Join-Path $captureGate.FullName ([Guid]::NewGuid().ToString('N'))
& "$($captureGate.FullName)/full-capture.exe" $captureOutput
if ($LASTEXITCODE -ne 0) { throw 'Full capture gate failed' }
Write-Host "Fixtures: $captureOutput"
