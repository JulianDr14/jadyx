# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Real-GPU component gate for D3D12Reaper, TrimEmptyHeaps, and NotZeroed 64 KiB buffer round-trip.
$ErrorActionPreference = 'Stop'
$testVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$testVs) { throw 'MSVC not found' }
$testNinja = Get-Content build-uwp/build.ninja
$testObject = ($testNinja | Select-String -SimpleMatch 'build src\video_core\CMakeFiles\video_core.dir\renderer_d3d12\d3d12_image_copy_blit.cpp.obj:').LineNumber
if (!$testObject) { throw 'Configured D3D12 object missing' }
$testIncludes = ($testNinja[$testObject..($testObject+6)] | Where-Object { $_ -match '^  INCLUDES =' }) -replace '^  INCLUDES = ', ''
$testLibs = ($testNinja | Where-Object { $_ -match '^  LINK_LIBRARIES = .*video_core.lib' } | Select-Object -Last 1) -replace '^  LINK_LIBRARIES = ', ''
if (!$testIncludes -or !$testLibs) { throw 'Configured includes or libraries missing' }
$testDir = New-Item -ItemType Directory -Force build-uwp/scheduler-reaper-buffer-gate
$testSource = Join-Path (Get-Location) 'tools/xbox/tests/scheduler-reaper-buffer.cpp'
$testVma = if (Test-Path 'build-uwp/src/jadyx/CMakeFiles/jadyx.dir/app/vma_impl.cpp.obj') { ' src\jadyx\CMakeFiles\jadyx.dir\app\vma_impl.cpp.obj' } else { '' }
$testCommands = @('@echo off', ('call "' + $testVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1', ('cd /d "' + (Join-Path (Get-Location) 'build-uwp') + '"'),
    ('cl /nologo /O2 /MD /Gy /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK ' + $testIncludes + ' "' + $testSource + '"' + $testVma + ' /Fo:scheduler-reaper-buffer-gate\ /Fe:scheduler-reaper-buffer-gate\scheduler-reaper-buffer.exe /link /OPT:REF /NODEFAULTLIB:libcmt ' + ' d3dcompiler.lib ' + $testLibs))
Set-Content "$($testDir.FullName)/build.cmd" $testCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($testDir.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Scheduler reaper buffer gate build failed' }
$testOldPath = $env:PATH
try {
    $testAppCrt = Get-AppxPackage 'Microsoft.VCLibs.140.00' | Where-Object Architecture -eq 'X64' | Select-Object -First 1 -ExpandProperty InstallLocation
    $env:PATH = (Join-Path (Get-Location) 'build-uwp/package/layout') + ';' + $testAppCrt + ';' + $env:PATH
    & "$($testDir.FullName)/scheduler-reaper-buffer.exe"
} finally {
    $env:PATH = $testOldPath
}
if ($LASTEXITCODE -ne 0) { throw 'Scheduler reaper buffer GPU gate failed' }
