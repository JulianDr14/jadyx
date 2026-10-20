# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Desktop harness with production OpenSSL and Windows.Web.Http; not a Series deployment gate.
[CmdletBinding()]
param([string]$CandidateDirectory)
$ErrorActionPreference='Stop'
$gateVs=& "${env:ProgramFiles(x86)}/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(-not $gateVs){throw 'MSVC not found'}
$gateDir=New-Item -ItemType Directory -Force build-uwp/test-results/updater
$includes='/I src /I .cache/cpm/nlohmann_json/v3.12.0/include /I .cache/cpm/fmt/12.1.0/include /I .cache/cpm/openssl/openssl-3.6.2/include /I build-uwp/_deps/openssl-build/include /I .cache/cpm/libarchive/v3.8.9/libarchive'
$command=@('@echo off',('call "'+$gateVs+'\VC\Auxiliary\Build\vcvarsall.bat" x64'),'if errorlevel 1 exit /b 1')
foreach($name in 'policy','http','service','e2e'){
    $extra=if($name -ne 'http'){'src/jadyx/updates/update_policy.cpp'}else{''}
    if($name -in @('service','e2e')){$extra+=' src/jadyx/updates/package_deployment.cpp src/jadyx/updates/package_validation.cpp src/jadyx/updates/update_storage.cpp'}
    $libraries=if($name -in @('service','e2e')){'build-uwp/externals/libarchive/archive_read.lib build-uwp/_deps/xz-build/lzma.lib build-uwp/_deps/zlib-build/zs.lib build-uwp/_deps/zstd-build/lib/zstd_static.lib'}else{''}
    $command+="cl /nologo /O2 /EHsc /std:c++20 /utf-8 /MD /DNOMINMAX /DWIN32_LEAN_AND_MEAN /DLIBARCHIVE_STATIC $includes tools/xbox/tests/updater-$name.cpp $extra /Fe:build-uwp/test-results/updater/$name.exe /Fo:build-uwp/test-results/updater/ /link build-uwp/_deps/openssl-build/libcrypto.lib WindowsApp.lib user32.lib $libraries"
    $command+='if errorlevel 1 exit /b 1'
}
$command+='build-uwp\test-results\updater\policy.exe','if errorlevel 1 exit /b 1',
    'python tools/xbox/tests/updater-http.py build-uwp/test-results/updater/http.exe','if errorlevel 1 exit /b 1',
    'python tools/xbox/tests/release.py','if errorlevel 1 exit /b 1',
    'python tools/xbox/tests/updater-packages.py','if errorlevel 1 exit /b 1',
    'build-uwp\test-results\updater\service.exe','if errorlevel 1 exit /b 1'
Set-Content -LiteralPath "$($gateDir.FullName)/run.cmd" -Value $command -Encoding ascii
& "$env:SystemRoot/System32/cmd.exe" /d /c "$($gateDir.FullName)/run.cmd"
if($LASTEXITCODE -ne 0){throw 'Updater gates failed'}
& node tools/xbox/tests/updater-page.cjs
if($LASTEXITCODE -ne 0){throw 'Updater browser gates failed'}
if($CandidateDirectory){
    & python tools/xbox/tests/updater-e2e.py build-uwp/test-results/updater/e2e.exe $CandidateDirectory
    if($LASTEXITCODE -ne 0){throw 'Signed candidate end-to-end gate failed'}
    & python tools/xbox/tests/updater-appx-integrity.py (Join-Path $CandidateDirectory 'Jadyx_0.3.6.0_x64.appx')
    if($LASTEXITCODE -ne 0){throw 'Signed candidate integrity gates failed'}
}
