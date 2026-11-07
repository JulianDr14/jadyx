# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds and runs the amiibo store test against the real amiibo_crypto.cpp, from the repo root.
# Needs build-uwp built first (common.lib, fmt.lib, OpenSSL).
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
Set-Location $repo
$out = 'build-uwp/test-results/amiibo-store'
New-Item -ItemType Directory -Force $out | Out-Null
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC not found' }
$openssl = Get-ChildItem '.cache/cpm/openssl/*-windows-amd64' -Directory | Select-Object -First 1
if (!$openssl) { throw 'OpenSSL not found; configure build-uwp first' }
$include = '/I src /I build-uwp/src /I .cache/cpm/fmt/12.1.0/include /I "' + $openssl.FullName + '\include"'
$sources = 'tools\xbox\tests\amiibo-store.cpp src\jadyx\amiibo\amiibo_store.cpp src\core\hle\service\nfc\common\amiibo_crypto.cpp'
$libs = 'build-uwp/src/common/common.lib build-uwp/_deps/fmt-build/fmt.lib "' + $openssl.FullName + '\lib\libcrypto.lib" OneCore.lib ntdll.lib synchronization.lib ws2_32.lib crypt32.lib advapi32.lib'
$lines = @('@echo off', ('call "' + $vs + '\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul'), 'if errorlevel 1 exit /b 1',
    ('cl /nologo /utf-8 /std:c++20 /EHsc /MD /O1 /wd4324 /DARCHITECTURE_x86_64 /DNOMINMAX ' + $include + ' ' + $sources + ' /Fo:' + $out + '/ /Fe:' + $out + '/test.exe /link ' + $libs),
    'if errorlevel 1 exit /b 1', 'build-uwp\test-results\amiibo-store\test.exe', 'exit /b %errorlevel%')
Set-Content "$out/build.cmd" $lines -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c (Join-Path $repo "build-uwp\test-results\amiibo-store\build.cmd")
if ($LASTEXITCODE -ne 0) { throw 'Amiibo store gate failed' }
