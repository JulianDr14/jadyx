# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds and runs one of the renderer's standalone unit tests in tools\xbox\tests that has no run
# script of its own. suite.py runs each as a gate (unit-<name>); by hand, from the repo root:
#
#   powershell -ExecutionPolicy Bypass -File tools\xbox\tests\renderer-suite\run-unit.ps1 -Name bc45-decode
#
# The ones linking common.lib need build-uwp built first.

param([Parameter(Mandatory)] [string] $Name)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path

$base = '/nologo /utf-8 /std:c++20 /EHsc /MD /O2 /W4 /I src'
$common = '/wd4324 /DARCHITECTURE_x86_64 /DNOMINMAX /I build-uwp/src /I .cache/cpm/fmt/12.1.0/include'
$commonLibs = '/link build-uwp/src/common/common.lib build-uwp/_deps/fmt-build/fmt.lib OneCore.lib ntdll.lib synchronization.lib'
$units = @{
    'bc45-decode'         = @{ flags = '/I externals/bc_decoder'; sources = 'externals/bc_decoder/bc_decoder.cpp'; link = '' }
    'cache-pressure'      = @{ flags = ''; sources = ''; link = '' }
    'descriptor-copy'     = @{ flags = ''; sources = ''; link = '/link d3d12.lib dxgi.lib' }
    'linked-shader-cache' = @{ flags = ''; sources = ''; link = '' }
    'memory-guard'        = @{ flags = ''; sources = ''; link = '' }
    'overlay-memory'      = @{ flags = $common; sources = ''; link = '/link build-uwp/_deps/fmt-build/fmt.lib' }
    'pipeline-residency'  = @{ flags = ''; sources = ''; link = '' }
    'prewarm-budget'      = @{ flags = ''; sources = ''; link = '' }
    'fsr-plan'            = @{ flags = '/Zc:preprocessor /I externals/FidelityFX-FSR/ffx-fsr'; sources = 'src/video_core/fsr.cpp'; link = '' }
    'command-recorder'    = @{ flags = $common; sources = ''; link = $commonLibs }
    'spsc-wake-queue'     = @{ flags = $common; sources = ''; link = $commonLibs }
}
if (-not $units.ContainsKey($Name)) { throw "unknown unit '$Name'; known: $($units.Keys -join ', ')" }
$unit = $units[$Name]

$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'MSVC not found' }
$out = "build-uwp/renderer-suite/units/$Name"
New-Item -ItemType Directory -Force (Join-Path $repo $out) | Out-Null
$exe = "build-uwp\renderer-suite\units\$Name\test.exe"
$lines = @('@echo off', "call `"$vs\VC\Auxiliary\Build\vcvarsall.bat`" x64 >nul", 'if errorlevel 1 exit /b 1',
    "cd /d `"$repo`"",
    "cl $base $($unit.flags) tools/xbox/tests/$Name.cpp $($unit.sources) /Fo:$out/ /Fe:$out/test.exe $($unit.link)",
    'if errorlevel 1 exit /b 1', $exe, 'exit /b %errorlevel%')
$script = Join-Path $repo "$out/run.cmd"
Set-Content $script $lines -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c $script
if ($LASTEXITCODE -ne 0) { throw "unit $Name failed ($LASTEXITCODE)" }
