# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Builds the renderer suite's guest payload: an NRO on the deko3d samples' framework, whose shaders go
# through devkitPro's uam. Everything is copied into build-uwp\payloads\renderer-suite (outside git).
#
#   powershell -ExecutionPolicy Bypass -File tools\xbox\tests\renderer-suite\build-payload.ps1
#
# Needs devkitPro with switch-dev and deko3d. Output: build-uwp\payloads\renderer-suite\renderer_suite.nro

param(
    [string] $DevkitPro = "C:\devkitPro"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
$examplesSrc = Join-Path $DevkitPro "examples\switch\graphics\deko3d\deko_examples"
if (-not (Test-Path (Join-Path $examplesSrc "Makefile"))) { throw "deko3d examples not found in $examplesSrc" }
$make = Join-Path $DevkitPro "msys2\usr\bin\make.exe"
if (-not (Test-Path $make)) { throw "make not found: $make" }

$work = Join-Path $repo "build-uwp\payloads\renderer-suite"
$source = Join-Path $work "source"
New-Item -ItemType Directory -Force $source | Out-Null
New-Item -ItemType Directory -Force (Join-Path $work "romfs") | Out-Null
Copy-Item (Join-Path $examplesSrc "Makefile") $work -Force
Copy-Item (Join-Path $examplesSrc "source\SampleFramework") $source -Recurse -Force

# Its startup code and framework come as they are; what is ours replaces the examples. Removed files
# would otherwise stay and be built, so the copies are mirrored.
Get-ChildItem $source -File | Remove-Item -Force
$payload = Join-Path $PSScriptRoot "payload"
Get-ChildItem $payload -File | Copy-Item -Destination $source -Force
Get-ChildItem (Join-Path $payload "shaders") -File | Copy-Item -Destination $source -Force

$env:DEVKITPRO = "/opt/devkitpro"
$env:TMP = $work; $env:TEMP = $work; $env:TMPDIR = $work
$env:PATH = "$DevkitPro\msys2\usr\bin;$DevkitPro\devkitA64\bin;$DevkitPro\tools\bin;$env:PATH"

Push-Location $work
try {
    & $make -j8 "TARGET=renderer_suite" "BUILD=build" "APP_TITLE=renderer_suite"
    if ($LASTEXITCODE) { throw "make failed" }
    Write-Host "built: $(Join-Path $work 'renderer_suite.nro')"
} finally {
    Pop-Location
}
