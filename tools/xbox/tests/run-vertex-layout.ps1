# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Standalone gate of the D3D12 vertex layout plan and the repack shader model; no game data needed.
$ErrorActionPreference = 'Stop'
$gateVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$gateVs) { throw 'MSVC not found' }
$gateDir = New-Item -ItemType Directory -Force build-uwp/vertex-layout-gate
$gateCommands = @('@echo off', ('call "' + $gateVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1',
    'cl /nologo /O2 /MD /std:c++20 /Zc:preprocessor /EHsc /utf-8 /W4 /Isrc tools\xbox\tests\vertex-layout.cpp src\video_core\renderer_d3d12\d3d12_vertex_layout.cpp /Fobuild-uwp\vertex-layout-gate\ /Febuild-uwp\vertex-layout-gate\vertex-layout.exe',
    'if errorlevel 1 exit /b 1', 'build-uwp\vertex-layout-gate\vertex-layout.exe')
Set-Content "$($gateDir.FullName)/run.cmd" $gateCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($gateDir.FullName)/run.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Vertex layout gate failed' }
