# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
$ErrorActionPreference = 'Stop'
$menuGate = New-Item -ItemType Directory -Force build-uwp/game-menu-gate
$menuVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$menuVs) { throw 'MSVC not found' }
$menuCommands = @('@echo off', ('call "' + $menuVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'), 'if not "%errorlevel%"=="0" exit /b 1')
foreach ($menuTest in @('game-menu', 'localization-catalogs')) {
    $menuCommands += ('cl /nologo /O2 /MD /std:c++20 /EHsc /utf-8 /Zc:preprocessor /DJADYX_NO_WINRT /I src tools\xbox\tests\' + $menuTest + '.cpp src\jadyx\i18n\localization.cpp src\jadyx\i18n\locales\en.cpp src\jadyx\i18n\locales\es.cpp src\jadyx\i18n\locales\pt_br.cpp src\jadyx\i18n\locales\zh_cn.cpp src\jadyx\i18n\locales\registry.cpp /Fo:build-uwp\game-menu-gate\ /Fe:build-uwp\game-menu-gate\' + $menuTest + '.exe')
    $menuCommands += 'if not "%errorlevel%"=="0" exit /b 1'
    $menuCommands += ('build-uwp\game-menu-gate\' + $menuTest + '.exe')
    $menuCommands += 'if not "%errorlevel%"=="0" exit /b 1'
}
Set-Content "$($menuGate.FullName)/build.cmd" $menuCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($menuGate.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Game menu/localization gate failed' }
