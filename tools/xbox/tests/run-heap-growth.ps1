# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Desktop policy/packing and real D3D12 debug gates, without game data or app deployment.
$ErrorActionPreference = 'Stop'
$growthVs = & "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$growthVs) { throw 'MSVC not found' }
$growthDir = New-Item -ItemType Directory -Force test-results/hyrule-memory
$growthCommands = @('@echo off', ('call "' + $growthVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1')
foreach ($growthGate in 'heap-growth', 'heap-packing') {
    $growthLibs = if ($growthGate -eq 'heap-growth') { ' /link d3d12.lib dxgi.lib dxguid.lib' } else { '' }
    $growthCommands += "cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /W4 /WX /Isrc tools\xbox\tests\$growthGate.cpp /Fotest-results\hyrule-memory\ /Fetest-results\hyrule-memory\$growthGate.exe$growthLibs"
    $growthCommands += 'if errorlevel 1 exit /b 1'
    $growthCommands += "test-results\hyrule-memory\$growthGate.exe"
    $growthCommands += 'if errorlevel 1 exit /b 1'
}
Set-Content "$($growthDir.FullName)/run.cmd" $growthCommands -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($growthDir.FullName)/run.cmd" 2>&1 |
    Tee-Object -FilePath "$($growthDir.FullName)/gates.txt"
if ($LASTEXITCODE -ne 0) { throw 'heap growth gates failed' }
