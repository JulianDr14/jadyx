# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Emulated MIN/MAX sampler reduction test bench on the production UWP libraries. Run after the
# incremental build. Arguments go to the bench: a category (sanity, functional, regression,
# performance, stability), --seed N and/or --iterations N.
$ErrorActionPreference = 'Stop'
$testVs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$testVs) { throw 'MSVC not found' }
$testGlslang = Get-Command glslangValidator -ErrorAction SilentlyContinue | Select-Object -First 1 -ExpandProperty Source
if (!$testGlslang -and $env:VULKAN_SDK) { $testGlslang = Join-Path $env:VULKAN_SDK 'Bin\glslangValidator.exe' }
if (!$testGlslang -or !(Test-Path $testGlslang)) { $testGlslang = 'C:\glslang\bin\glslangValidator.exe' }
if (!(Test-Path $testGlslang)) { throw 'glslangValidator not found' }
$testNinja = Get-Content build-uwp/build.ninja
$testObject = ($testNinja | Select-String -SimpleMatch 'build src\video_core\CMakeFiles\video_core.dir\renderer_d3d12\d3d12_image_copy_blit.cpp.obj:').LineNumber
if (!$testObject) { throw 'Configured D3D12 object missing' }
$testIncludes = ($testNinja[$testObject..($testObject+6)] | Where-Object { $_ -match '^  INCLUDES =' }) -replace '^  INCLUDES = ', ''
$testLibs = ($testNinja | Where-Object { $_ -match '^  LINK_LIBRARIES = .*video_core.lib' } | Select-Object -Last 1) -replace '^  LINK_LIBRARIES = ', ''
if (!$testIncludes -or !$testLibs) { throw 'Configured includes or libraries missing' }
$testDir = New-Item -ItemType Directory -Force build-uwp/reduction-sampling-gate
$testSpv = New-Item -ItemType Directory -Force (Join-Path $testDir.FullName 'spv')
$testShaders = Join-Path (Get-Location) 'tools/xbox/tests/reduction-sampling'
$testVariants = @()
foreach ($dim in 1..4) { foreach ($op in 0..3) { $testVariants += ,@('comp', "comp_d${dim}_o${op}.spv", 'sample.comp', "-DDIM=$dim", "-DOP=$op") } }
$testVariants += ,@('comp', 'comp_int.spv', 'sample.comp', '-DDIM=2', '-DOP=0', '-DINTEGER=1')
$testVariants += ,@('vert', 'vs.spv', 'fullscreen.vert')
$testVariants += ,@('frag', 'fs_tex.spv', 'sample.frag')
$testVariants += ,@('frag', 'fs_bias.spv', 'sample.frag', '-DBIAS=1')
foreach ($variant in $testVariants) {
    $testArgs = [System.Collections.Generic.List[string]]@('-V', '-S', $variant[0])
    for ($i = 3; $i -lt $variant.Count; ++$i) { $testArgs.Add($variant[$i]) }
    $testArgs.AddRange([string[]]@('-o', (Join-Path $testSpv.FullName $variant[1]), (Join-Path $testShaders $variant[2])))
    # glslangValidator echoes the file name to stderr, which Windows PowerShell turns into an error.
    $testOutput = & {
        $ErrorActionPreference = 'Continue'
        & $testGlslang $testArgs.ToArray() 2>&1
    } | Out-String
    if ($LASTEXITCODE -ne 0) { throw "glslangValidator failed on $($variant[1]):`n$testOutput" }
}
$testSource = Join-Path (Get-Location) 'tools/xbox/tests/reduction-sampling.cpp'
$testCommands = @('@echo off', ('set "PATH=%PATH%;' + ${env:ProgramFiles(x86)} + '\Microsoft Visual Studio\Installer"'), ('call "' + $testVs + '\VC\Auxiliary\Build\vcvarsall.bat" x64'),
    'if errorlevel 1 exit /b 1', ('cd /d "' + (Join-Path (Get-Location) 'build-uwp') + '"'),
    ('cl /nologo /O2 /MD /Gy /std:c++20 /Zc:preprocessor /EHsc /utf-8 /DNOMINMAX /DBOOST_CONTAINER_NO_LIB /DBOOST_CONTAINER_STATIC_LINK ' + $testIncludes + ' "' + $testSource + '" src\jadyx\CMakeFiles\jadyx.dir\app\vma_impl.cpp.obj /Fo:reduction-sampling-gate\ /Fe:reduction-sampling-gate\reduction-sampling.exe /link /OPT:REF /NODEFAULTLIB:libcmt ' + ' d3dcompiler.lib ' + $testLibs))
Set-Content "$($testDir.FullName)/build.cmd" $testCommands -Encoding ascii
& "$env:SystemRoot\System32\cmd.exe" /d /c "$($testDir.FullName)\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Reduction sampling bench build failed' }
$testOldPath = $env:PATH
try {
    $testAppCrt = Get-AppxPackage 'Microsoft.VCLibs.140.00' | Where-Object Architecture -eq 'X64' | Select-Object -First 1 -ExpandProperty InstallLocation
    # The freshly built spirv_to_dxil first: the packaged copy may predate EMULATE_REDUCTION.
    $testSpirvToDxil = Join-Path (Get-Location) '../mesa-build/build-uwp/src/microsoft/spirv_to_dxil'
    $env:PATH = $testSpirvToDxil + ';' + (Join-Path (Get-Location) 'build-uwp/package/layout') + ';' + $testAppCrt + ';' + $env:PATH
    & "$($testDir.FullName)/reduction-sampling.exe" --spv $testSpv.FullName @args
} finally {
    $env:PATH = $testOldPath
}
if ($LASTEXITCODE -ne 0) { throw 'Reduction sampling bench failed' }
