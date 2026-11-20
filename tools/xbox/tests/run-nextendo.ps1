# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Run from the repository root. Desktop gate for the Nextendo modules that need no server.
# Optional: set NEXTENDO_CATALOG_SAMPLE to a downloaded /service-catalog/v1/game-catalog response
# to also check the real signature.
param([string]$BuildDir = 'build-uwp-nolto')
$ErrorActionPreference = 'Stop'
$gate = New-Item -ItemType Directory -Force build-uwp/nextendo-gate
$crypto = "$BuildDir/_deps/openssl-build/libcrypto.lib"
if (!(Test-Path $crypto)) { throw "libcrypto.lib not found under $BuildDir; build the UWP tree first" }
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'MSVC not found' }
$includes = "/I src /I .cache/cpm/nlohmann_json/v3.12.0/include /I .cache/cpm/openssl/openssl-3.6.2/include /I $BuildDir/_deps/openssl-build/include"
$sources = @(
    'tools\xbox\tests\nextendo.cpp',
    'src\core\online\nextendo\address_book.cpp',
    'src\core\online\nextendo\catalog.cpp',
    'src\core\online\nextendo\config.cpp',
    'src\core\online\nextendo\encoding.cpp',
    'src\core\online\nextendo\host_rules.cpp',
    'src\core\online\nextendo\id_token.cpp',
    'src\core\online\nextendo\trust_store.cpp',
    'src\jadyx\network\nextendo\api_types.cpp',
    'src\jadyx\network\nextendo\bcat_package.cpp',
    'src\jadyx\network\nextendo\catalog_rows.cpp',
    'src\jadyx\network\nextendo\link_stats.cpp',
    'src\jadyx\network\nextendo\pkce.cpp',
    'src\jadyx\network\nextendo\preferences.cpp',
    'src\jadyx\network\nextendo\session.cpp'
) -join ' '
$commands = @(
    '@echo off',
    'set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"',
    ('call "' + $vs + '\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul'),
    'if not "%errorlevel%"=="0" exit /b 1',
    "cl /nologo /O2 /MD /std:c++20 /EHsc /W4 /WX /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS $includes $sources /Fo:build-uwp\nextendo-gate\ /Fe:build-uwp\nextendo-gate\nextendo.exe /link $crypto ws2_32.lib crypt32.lib advapi32.lib user32.lib bcrypt.lib",
    'if not "%errorlevel%"=="0" exit /b 1',
    'build-uwp\nextendo-gate\nextendo.exe',
    'exit /b %errorlevel%'
)
Set-Content "$($gate.FullName)/build.cmd" $commands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($gate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw "Nextendo gate failed (exit $LASTEXITCODE)" }
