# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# JIT profile prewarm gate: precompile, profiles, parallel owners and the shared-cache warm budget.
# Run from the repo root after building jadyx in build-uwp-nolto, which builds the production
# Dynarmic library this links; it also needs the configured fmt and xbyak sources.
param([string]$BuildDir = 'build-uwp-nolto')
$ErrorActionPreference = 'Stop'
$jitVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$jitVs) { throw 'MSVC not found' }
$jitBoost = Get-ChildItem '.cache/cpm/boost' -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$jitBoost) { throw 'Bundled Boost not found' }
$jitIncludes = @(Get-ChildItem "$($jitBoost.FullName)/libs" -Directory | ForEach-Object { '/I "' + $_.FullName + '\include"' })
$jitCache = Get-Content "$BuildDir/CMakeCache.txt"
function Get-PackageDir([string]$name) {
    $line = $jitCache | Where-Object { $_.StartsWith("CPM_PACKAGE_${name}_SOURCE_DIR:INTERNAL=") } | Select-Object -First 1
    if (!$line) { throw "Configured $name source missing" }
    return $line.Substring($line.IndexOf('=') + 1)
}
$jitIncludes += '/I "' + (Get-PackageDir 'fmt') + '/include"'
$jitIncludes += '/I "' + (Get-PackageDir 'xbyak') + '"'
$jitDense = Get-ChildItem '.cache/cpm/unordered_dense' -Directory | Select-Object -First 1
if (!$jitDense) { throw 'Bundled unordered_dense not found' }
$jitIncludes += '/I "' + $jitDense.FullName + '\include"'
$jitOut = "$BuildDir\diagnostics\jit-prewarm"
$jitDir = New-Item -ItemType Directory -Force $jitOut
# The backend headers must see the library's own defines, or their types differ from the library's.
$jitDefines = '/DARCHITECTURE_x86_64=1 /DDYNARMIC_ENABLE_CPU_FEATURE_DETECTION=1 /DDYNARMIC_ENABLE_NO_EXECUTE_SUPPORT=1 ' +
    '/DDYNARMIC_UWP_APPCONTAINER=1 /DMCL_IGNORE_ASSERTS=1 /DXBYAK_OLD_DISP_CHECK=1 /DXBYAK_STRICT_CHECK_MEM_REG_SIZE=0 ' +
    '/DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK /DWINVER=0x0A00 /D_WIN32_WINNT=0x0A00 /GR- '
$jitCompile = 'cl /nologo /O2 /MD /UNDEBUG /std:c++20 /utf-8 /EHsc /DNOMINMAX ' + $jitDefines + '/I src /I src/dynarmic/src /I ' + $BuildDir + '/src ' + ($jitIncludes -join ' ') +
    ' tools\xbox\tests\jit-prewarm.cpp /Fo:' + $jitOut + '\ /Fe:' + $jitOut + '\test.exe' +
    ' /link ' + $BuildDir + '\src\dynarmic\src\dynarmic\dynarmic.lib ' + $BuildDir + '\src\common\common.lib ' + $BuildDir + '\_deps\fmt-build\fmt.lib OneCore.lib WindowsApp.lib'
$jitCommands = @('@echo off', ('call "' + $jitVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if errorlevel 1 exit /b 1', $jitCompile)
Set-Content "$($jitDir.FullName)/run.cmd" -Value $jitCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($jitDir.FullName)\run.cmd"
if ($LASTEXITCODE -ne 0) { throw 'jit-prewarm build failed' }
& "$($jitDir.FullName)/test.exe"
# A failed assert aborts with a code errorlevel reads as success: check the real one.
if ($LASTEXITCODE -ne 0) { throw "jit-prewarm gate failed (exit $LASTEXITCODE)" }
