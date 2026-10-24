# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stages, packs and signs the Phase-2 headless boot appx (src/jadyx) for sideloading
# onto an Xbox Series X|S in Dev Mode. See docs/xbox/xbox_deploy.md for the full flow.
#
#   .\tools\xbox\package-appx.ps1 -BootNro C:\path\to\boot.nro
#
# Produces build-uwp\package\jadyx.appx plus the .cer you must install on the console.

[CmdletBinding()]
param(
    # Directory the uwp-x64 preset built into (CMakePresets.json binaryDir).
    [string] $BuildDir = "build-uwp",
    # Homebrew NRO bundled as the GATE-2 payload. It must emit the JIT-liveness sentinel
    # JADYX_JIT_ALIVE via svcOutputDebugString. NO keys/firmware/commercial ROMs (house rule).
    [string] $BootNro,
    # For payloads without the sentinels (deko3d examples, ...): run the NRO this many seconds,
    # then shut down. 0 keeps the sentinel-driven boot. Written to boot.cfg in the package.
    [int] $RunSeconds = 0,
    # Enables the D3D12 debug layer (PC only: needs the Graphics Tools optional feature).
    [switch] $DebugLayer,
    # The user's own dumps for a game test, bundled under userdata\ and copied into LocalState by
    # the app on its first run (uwp_boot.cpp SeedUserData). They only ever go into this local
    # package: never commit them, never publish the appx. LocalState survives package updates, so
    # once seeded, later packages can pass just -Game <file name> without the file.
    #   -Game      game dump (.nsp/.xci) to boot instead of boot.nro, or the name of one already seeded
    [string] $Game,
    # A script kept outside the repo, run with -Layout <package layout folder> before packing: local
    # additions to a test package that never go into the repo.
    [string] $StageScript,
    # Folder library for PC/Series; game files live under the app's LocalState\games.
    [switch] $Library,
    # Diagnostic builds: the in-process sampling profiler (boot.cfg profile=1). "full" also turns on
    # the per-draw GPU and per-core CPU timers, which slow the GPU thread. Never for a release.
    [ValidateSet("", "on", "full")]
    [string] $Profile = "",
    # Diagnostic builds: "Capture performance" in the game menu (boot.cfg hunt=1), for
    # tools/xbox/hunt-report.py. Costs nothing until a capture is asked for.
    [switch] $Hunt,
    # Diagnostic builds: "Capture graphics" in the game menu (boot.cfg material_capture_menu=1),
    # the controller F7: traces the draws of the next frames into the log.
    [switch] $GraphicsCapture,
    # Extra boot.cfg lines for diagnosis: "log_filter=*:Info HW.GPU:Debug", "renderer=null".
    [string[]] $BootCfg = @(),
    # Must match Identity/@Publisher in dist/uwp/AppxManifest.xml, character for character.
    [string] $PublisherCN = "CN=JadyxDev",
    # Mesa's SPIR-V -> DXIL translator for the D3D12 renderer, built by build-spirv-to-dxil.ps1.
    # Without it the renderer still presents, through its CPU fallback.
    [string] $SpirvToDxil = "..\mesa-build\build-uwp\src\microsoft\spirv_to_dxil\spirv_to_dxil.dll",
    # Optional local diagnostic DXIL sidecars; never bundled by default.
    [string] $MaterialProbeShaders,
    [ValidateRange(6, 8)]
    [int] $MaterialProbeShaderCount = 6,
    # Sidecars expose original CFG intermediates instead of the authored kernels.
    [switch] $MaterialProbeOriginalCfg,
    [switch] $MaterialProbeBranchControls,
    [string] $OutDir = "build-uwp\package",
    # Public release (.github/workflows/release.yml): the plain library package. Refuses every
    # payload, user-data and diagnostic option, and fails instead of warning when a runtime piece
    # (shader DLLs, VCLibs) is missing.
    [switch] $Release,
    # Package version for the staged manifest (MAJOR.MINOR.BUILD.REVISION). The manifest in the
    # repo is left as it is.
    [string] $Version,
    # Signing certificate as a PFX file (CI), its password in $env:JADYX_PFX_PASSWORD. Without it
    # the CurrentUser\My certificate for $PublisherCN is used, or minted.
    [string] $PfxPath
)

$ErrorActionPreference = "Stop"
if ($Release) {
    if ($BootNro -or $Game -or $StageScript -or $Profile -or $Hunt -or $GraphicsCapture -or
        $DebugLayer -or $MaterialProbeShaders -or $RunSeconds -gt 0 -or $BootCfg.Count -gt 0) {
        throw 'A release package takes no payload, user data or diagnostic options'
    }
    $Library = [switch]$true
}
if ($Version -and $Version -notmatch '^\d{1,5}\.\d{1,5}\.\d{1,5}\.\d{1,5}$') {
    throw "Invalid package version '$Version': expected MAJOR.MINOR.BUILD.REVISION"
}
if ($MaterialProbeOriginalCfg -and (!$MaterialProbeShaders -or $MaterialProbeShaderCount -ne 8)) {
    throw 'Original CFG diagnosis requires eight explicit sidecars'
}
if ($MaterialProbeBranchControls -and !$MaterialProbeOriginalCfg) {
    throw 'Branch controls require the explicit original CFG diagnostic mode'
}
if ($Library) { $BootCfg += 'library=1'; $BootCfg += 'play=1' }
if ($Profile -eq "on") { $BootCfg += 'profile=1' }
if ($Profile -eq "full") { $BootCfg += 'profile=full' }
if ($Hunt) { $BootCfg += 'hunt=1' }
if ($GraphicsCapture) { $BootCfg += 'material_capture_menu=1' }
$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $repo

function Find-SdkTool([string] $name) {
    $roots = @("${env:ProgramFiles(x86)}\Windows Kits\10\bin", "${env:ProgramFiles}\Windows Kits\10\bin")
    $hit = $roots | Where-Object { Test-Path $_ } | ForEach-Object {
        Get-ChildItem $_ -Recurse -Filter $name -ErrorAction SilentlyContinue |
            Where-Object { $_.DirectoryName -like '*\x64' }
    } | Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $hit) { throw "$name not found. Install the Windows 10/11 SDK (it ships MakeAppx + SignTool)." }
    return $hit.FullName
}

# --- 1. locate the built exe -------------------------------------------------------------
$exe = Join-Path $repo "$BuildDir\bin\jadyx.exe"
if (-not (Test-Path $exe)) {
    $found = Get-ChildItem (Join-Path $repo $BuildDir) -Recurse -Filter jadyx.exe -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if (-not $found) {
        throw "jadyx.exe not found under $BuildDir. Build it first from a 'vcvarsall.bat x64 uwp' shell:`n" +
              "  cmake --preset uwp-x64`n  cmake --build --preset uwp-x64 --target jadyx"
    }
    $exe = $found.FullName
}
Write-Host "exe      : $exe"

# --- 2. stage the package layout ---------------------------------------------------------
$layout = Join-Path $repo "$OutDir\layout"
if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
New-Item -ItemType Directory -Path $layout -Force | Out-Null

Copy-Item $exe $layout
Copy-Item (Join-Path $repo "dist\uwp\AppxManifest.xml") $layout
Copy-Item (Join-Path $repo "dist\uwp\Assets") $layout -Recurse
Copy-Item (Join-Path $repo "dist\compat") $layout -Recurse

# Any runtime DLLs the link produced land next to the exe; carry them along.
Get-ChildItem (Split-Path $exe) -Filter *.dll -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item $_.FullName $layout }

# The D3D12 renderer's shader path: spirv_to_dxil.dll translates, dxil.dll (Windows SDK, freely
# redistributable) signs the DXIL. Both are loaded at runtime from the package root.
$s2d = if ([IO.Path]::IsPathRooted($SpirvToDxil)) { $SpirvToDxil } else { Join-Path $repo $SpirvToDxil }
if (Test-Path $s2d) {
    Copy-Item $s2d $layout
    Write-Host "shaders  : $s2d"
    try {
        $dxil = Find-SdkTool "dxil.dll"
        Copy-Item $dxil $layout
        Write-Host "signing  : $dxil"
    } catch {
        if ($Release) { throw "dxil.dll not found in the Windows SDK: a release needs the GPU shader path." }
        Write-Warning "dxil.dll not found in the Windows SDK: the D3D12 renderer will present through the CPU."
    }
} else {
    if ($Release) { throw "spirv_to_dxil.dll not found ($s2d): a release needs the GPU shader path." }
    Write-Warning "spirv_to_dxil.dll not found ($s2d): the D3D12 renderer will present through the CPU."
}

if ($MaterialProbeShaders) {
    for ($probeIndex = 0; $probeIndex -lt $MaterialProbeShaderCount; ++$probeIndex) {
        $probeFile = Get-Item -LiteralPath (Join-Path $MaterialProbeShaders "material_reflection_$probeIndex.dxil")
        if ($probeFile.Length -lt 32 -or $probeFile.Length -gt 1MB) {
            throw "Invalid diagnostic sidecar size: $($probeFile.Name)"
        }
        Copy-Item -LiteralPath $probeFile.FullName -Destination $layout
    }
    if ($MaterialProbeOriginalCfg) {
        [IO.File]::WriteAllText((Join-Path $layout 'material_reflection_original_cfg.flag'), 'original-cfg-v1')
    }
    if ($MaterialProbeBranchControls) {
        [IO.File]::WriteAllText((Join-Path $layout 'material_reflection_branch_controls.flag'), 'branch-controls-v1')
    }
}

# User data (a game dump): see the parameters above.
$userdata = Join-Path $layout "userdata"
$gameName = $null
if ($Game) {
    $gameName = Split-Path -Leaf $Game
    if (Test-Path -LiteralPath $Game -PathType Leaf) {
        New-Item -ItemType Directory -Force "$userdata\games" | Out-Null
        Copy-Item -LiteralPath $Game "$userdata\games\$gameName"
        Write-Host "game     : $Game"
    } else {
        Write-Host "game     : $gameName (expected already in LocalState\games)"
    }
}
if ($StageScript) {
    & $StageScript -Layout $layout
    Write-Host "staged   : $StageScript"
}

# uwp_boot.cpp reads Package.InstalledLocation\boot.nro - the payload must sit at the layout root.
if ($BootNro -or $gameName -or $BootCfg.Count -gt 0) {
    if ($BootNro) {
        if (-not (Test-Path $BootNro)) { throw "BootNro not found: $BootNro" }
        Copy-Item $BootNro (Join-Path $layout "boot.nro")
        Write-Host "payload  : $BootNro -> boot.nro"
    }
    $cfg = ""
    if ($gameName) {
        $cfg += "game=$gameName`n"
    }
    # powershell -File hands "-BootCfg a=1,b=2" over as one string: split it here.
    foreach ($line in ($BootCfg | ForEach-Object { $_ -split ',' } | Where-Object { $_ })) {
        $cfg += "$line`n"
        Write-Host "boot.cfg : $line"
    }
    if ($RunSeconds -gt 0) {
        $cfg += "run_seconds=$RunSeconds`n"
        Write-Host "mode     : run $RunSeconds s (no sentinels)"
    }
    if ($DebugLayer) {
        $cfg += "debug_layer=1`n"
        Write-Host "debug    : D3D12 debug layer on"
    }
    if ($cfg) {
        [IO.File]::WriteAllText((Join-Path $layout "boot.cfg"), $cfg)
    }
    # Every diagnostic (DRED, profilers, capture menus, debug layer) is a boot.cfg line and off
    # without it; the bug tracker is on by default. A release carries the library and nothing else.
    if ($Release -and $cfg -ne "library=1`nplay=1`n") {
        throw "A release boot.cfg must hold only library=1 and play=1, not:`n$cfg"
    }
} else {
    Write-Warning "No -BootNro given. The app will activate, fail to load the NRO and log status 2 to jadyx_diag.txt."
}

# Publisher mismatch between manifest and cert is the #1 sideload rejection; fail loudly here.
[xml]$mf = Get-Content (Join-Path $layout "AppxManifest.xml") -Raw
if ($mf.Package.Identity.Publisher -ne $PublisherCN) {
    throw "Publisher mismatch: manifest has '$($mf.Package.Identity.Publisher)' but signing with '$PublisherCN'."
}
if ($Version) {
    $mf.Package.Identity.Version = $Version
    # XmlDocument.Save alone writes a UTF-8 BOM; updaters up to 0.3.7.0 reject such a manifest.
    $settings = New-Object System.Xml.XmlWriterSettings
    $settings.Encoding = New-Object System.Text.UTF8Encoding $false
    $settings.Indent = $true
    $writer = [System.Xml.XmlWriter]::Create((Join-Path $layout "AppxManifest.xml"), $settings)
    try { $mf.Save($writer) } finally { $writer.Dispose() }
}
Write-Host "version  : $($mf.Package.Identity.Version)"

# The tile/splash files carry their real scale (Square150x150Logo.scale-200.png is 300x300) while
# the manifest names the base file; resources.pri maps one to the other. Index a copy of Assets
# alone so a bundled game dump never goes through MakePri.
$makepri = Find-SdkTool "MakePri.exe"
$priRoot = Join-Path $repo "$OutDir\pri"
if (Test-Path $priRoot) { Remove-Item $priRoot -Recurse -Force }
New-Item -ItemType Directory -Path $priRoot -Force | Out-Null
Copy-Item (Join-Path $layout "Assets") $priRoot -Recurse
$priConfig = Join-Path $repo "$OutDir\priconfig.xml"
& $makepri createconfig /cf $priConfig /dq en-US /pv 10.0.0 /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "MakePri createconfig failed ($LASTEXITCODE)." }
# The default config splits scales into resources.scale-200.pri for a bundle's resource packages;
# a lone appx needs every candidate in its main resources.pri.
[xml]$priXml = Get-Content $priConfig -Raw
$priPackaging = $priXml.SelectSingleNode("//packaging")
if ($priPackaging) { [void]$priPackaging.ParentNode.RemoveChild($priPackaging) }
$priXml.Save($priConfig)
& $makepri new /pr $priRoot /cf $priConfig /mn (Join-Path $layout "AppxManifest.xml") `
    /of (Join-Path $layout "resources.pri") /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "MakePri failed ($LASTEXITCODE)." }

# --- 3. pack -----------------------------------------------------------------------------
$makeappx = Find-SdkTool "MakeAppx.exe"
$appx = Join-Path $repo "$OutDir\jadyx.appx"
if (Test-Path $appx) { Remove-Item $appx -Force }
# A bundled game dump is gigabytes of encrypted data that does not compress: skip compression.
$packArgs = @("pack", "/d", $layout, "/p", $appx, "/o")
if (Test-Path "$userdata\games") { $packArgs += "/nc" }
& $makeappx @packArgs
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed ($LASTEXITCODE)." }

# --- 4. sign -----------------------------------------------------------------------------
$signtool = Find-SdkTool "SignTool.exe"
$cer = Join-Path $repo "$OutDir\jadyx.cer"
if ($PfxPath) {
    # CI: the project's certificate comes from a secret, so every release has the same signer.
    if (-not $env:JADYX_PFX_PASSWORD) { throw "JADYX_PFX_PASSWORD is not set for $PfxPath." }
    $cert = New-Object Security.Cryptography.X509Certificates.X509Certificate2(
        (Resolve-Path $PfxPath).Path, $env:JADYX_PFX_PASSWORD)
    if ($cert.Subject -ne $PublisherCN) {
        throw "The PFX is issued to '$($cert.Subject)', not '$PublisherCN'."
    }
    & $signtool sign /fd SHA256 /f (Resolve-Path $PfxPath).Path /p $env:JADYX_PFX_PASSWORD `
        /t http://timestamp.digicert.com $appx
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed ($LASTEXITCODE)." }
    [IO.File]::WriteAllBytes($cer, $cert.Export([Security.Cryptography.X509Certificates.X509ContentType]::Cert))
} else {
    # The updater pins the distribution public key. Never silently mint a different signer.
    $cert = Get-ChildItem Cert:\CurrentUser\My |
            Where-Object { $_.Subject -eq $PublisherCN -and $_.NotAfter -gt (Get-Date) } |
            Select-Object -First 1
    if (-not $cert) {
        throw 'The Jadyx signing certificate is missing. Restore the distribution certificate; do not mint a replacement for an update.'
    }

    & $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /t http://timestamp.digicert.com $appx
    if ($LASTEXITCODE -ne 0) { throw "SignTool failed ($LASTEXITCODE)." }

    # The console must trust the signer: upload this .cer alongside the appx in the Device Portal.
    Export-Certificate -Cert $cert -FilePath $cer -Type CERT | Out-Null
}

# --- 5. the VCLibs framework package ------------------------------------------------------
# The exe hard-imports the Store CRT, which lives in the Microsoft.VCLibs.140.00 framework
# package rather than ours (see the PackageDependency in the manifest). The console needs it
# installed too, so hand it over next to our package instead of leaving the user to find it.
# It ships with the VS "C++ (v143) UWP tools" component, as an Extension SDK.
$vclibs = $null
foreach ($pf in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
    if (-not $pf) { continue }
    $cand = Join-Path $pf "Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx"
    if (Test-Path -LiteralPath $cand) { $vclibs = $cand; break }
}
if ($vclibs) {
    $vcOut = Join-Path $repo "$OutDir\Microsoft.VCLibs.x64.14.00.appx"
    Copy-Item -LiteralPath $vclibs -Destination $vcOut -Force
} else {
    if ($Release) { throw "Microsoft.VCLibs.x64.14.00.appx not found: a release must ship it." }
    Write-Warning ("Microsoft.VCLibs.x64.14.00.appx not found. The app will FAIL TO ACTIVATE on-console " +
                   "without it. Install the VS component Microsoft.VisualStudio.ComponentGroup.UWP.VC.")
}

Write-Host ""
Write-Host "package  : $appx"
Write-Host "cert     : $cer"
if ($vclibs) { Write-Host "framework: $vcOut  (upload as a dependency package)" }
Write-Host "Next: Device Portal https://<xbox-ip>:11443 -> Add -> upload all of the above -> set the app to Game mode."
