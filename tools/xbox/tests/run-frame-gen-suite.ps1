# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Frame generation behaviour suite (a report, not a gate): pacer cases for 15-20, 35-40 and 45-50
# fps games and double/triple buffer with dynamic vsync, GPU interpolation and whole-path runs
# (hardware GPU + debug layer), then GPU timing on the desktop GPU. Built against the production
# UWP libraries like run-frame-gen.ps1; run after the incremental build. Exit code 2 from the suite
# means it ran and some cases failed their criteria.
$ErrorActionPreference = 'Stop'
$testVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$testVs) { throw 'MSVC not found' }
$testNinja = Get-Content build-uwp/build.ninja
$testObject = ($testNinja | Select-String -SimpleMatch 'build src\video_core\CMakeFiles\video_core.dir\renderer_d3d12\d3d12_image_copy_blit.cpp.obj:').LineNumber
if (!$testObject) { throw 'Configured D3D12 object missing' }
$testIncludes = ($testNinja[$testObject..($testObject+6)] | Where-Object { $_ -match '^  INCLUDES =' }) -replace '^  INCLUDES = ', ''
$testLibs = ($testNinja | Where-Object { $_ -match '^  LINK_LIBRARIES = .*video_core.lib' } | Select-Object -Last 1) -replace '^  LINK_LIBRARIES = ', ''
if (!$testIncludes -or !$testLibs) { throw 'Configured includes or libraries missing' }
$testDir = New-Item -ItemType Directory -Force build-uwp/frame-gen-suite
$testSource = Join-Path (Get-Location) 'tools/xbox/tests/frame-gen-suite.cpp'
$testCommands = @('@echo off', ('call "' + $testVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1', ('cd /d "' + (Join-Path (Get-Location) 'build-uwp') + '"'),
    ('cl /nologo /O2 /MD /Gy /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK ' + $testIncludes + ' "' + $testSource + '" src\jadyx\CMakeFiles\jadyx.dir\app\vma_impl.cpp.obj /Fo:frame-gen-suite\ /Fe:frame-gen-suite\frame-gen-suite.exe /link /OPT:REF /NODEFAULTLIB:libcmt ' + $testLibs))
Set-Content "$($testDir.FullName)/build.cmd" $testCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($testDir.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Frame generation suite build failed' }
$testAppCrt = Get-AppxPackage 'Microsoft.VCLibs.140.00' | Where-Object Architecture -eq 'X64' | Select-Object -First 1 -ExpandProperty InstallLocation
$testOldPath = $env:PATH
try {
    $env:PATH = (Join-Path (Get-Location) 'build-uwp/package/layout') + ';' + $testAppCrt + ';' + $env:PATH
    & "$($testDir.FullName)/frame-gen-suite.exe"
    $testResult = $LASTEXITCODE
    & "$($testDir.FullName)/frame-gen-suite.exe" perf
    $testPerf = $LASTEXITCODE
} finally {
    $env:PATH = $testOldPath
}
if ($testResult -eq 1 -or $testPerf -ne 0) { throw 'Frame generation suite did not run' }
if ($testResult -eq 2) { Write-Host 'Frame generation suite: some cases failed their criteria (see SUMMARY)' }
