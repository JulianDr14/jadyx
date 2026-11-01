# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# JIT load table gate (jit_read_table): loads read the second half of the page table, stores the
# first, on A64 and A32. Run from the repo root after building jadyx in build-uwp, which builds
# the production Dynarmic library this links.
$ErrorActionPreference = 'Stop'
$jitVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$jitVs) { throw 'MSVC not found' }
$jitBoost = Get-ChildItem '.cache/cpm/boost' -Directory | Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$jitBoost) { throw 'Bundled Boost not found' }
$jitIncludes = Get-ChildItem "$($jitBoost.FullName)/libs" -Directory | ForEach-Object { '/I "' + $_.FullName + '\include"' }
$jitFmt = Get-Content 'build-uwp/CMakeCache.txt' | Where-Object { $_.StartsWith('CPM_PACKAGE_fmt_SOURCE_DIR:INTERNAL=') } | Select-Object -First 1
if (!$jitFmt) { throw 'Configured fmt source missing' }
$jitFmt = $jitFmt.Substring($jitFmt.IndexOf('=') + 1)
$jitDir = New-Item -ItemType Directory -Force 'build-uwp/diagnostics/jit-read-table'
$jitCompile = 'cl /nologo /O2 /MD /UNDEBUG /std:c++20 /utf-8 /EHsc /DNOMINMAX /I src /I src/dynarmic/src /I build-uwp/src /I "' + $jitFmt + '/include" ' + ($jitIncludes -join ' ') +
    ' tools\xbox\tests\jit-read-table.cpp /Fo:build-uwp\diagnostics\jit-read-table\ /Fe:build-uwp\diagnostics\jit-read-table\test.exe' +
    ' /link build-uwp\src\dynarmic\src\dynarmic\dynarmic.lib build-uwp\src\common\common.lib build-uwp\_deps\fmt-build\fmt.lib OneCore.lib WindowsApp.lib'
$jitCommands = @('@echo off', ('call "' + $jitVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if errorlevel 1 exit /b 1', $jitCompile)
Set-Content "$($jitDir.FullName)/run.cmd" -Value $jitCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($jitDir.FullName)\run.cmd"
if ($LASTEXITCODE -ne 0) { throw 'jit-read-table build failed' }
& "$($jitDir.FullName)/test.exe"
if ($LASTEXITCODE -ne 0) { throw 'jit-read-table gate failed' }
