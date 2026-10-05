# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Stages, packs and signs the Phase-2 headless boot appx (src/eden_uwp) for sideloading
# onto an Xbox Series X|S in Dev Mode. See docs/xbox_deploy.md for the full flow.
#
#   .\tools\xbox\package-appx.ps1 -BootNro C:\path\to\boot.nro
#
# Produces build-uwp\series\eden-xbox.appx plus its version and current symbols.

[CmdletBinding()]
param(
    # Directory the uwp-x64 preset built into (CMakePresets.json binaryDir).
    [string] $BuildDir = "build-uwp",
    # Homebrew NRO bundled as the GATE-2 payload. It must emit the JIT-liveness sentinel
    # EDEN_XBOX_JIT_ALIVE via svcOutputDebugString. NO keys/firmware/commercial ROMs (house rule).
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
    # Folder library for PC/Series; game files live under the app's LocalState\games.
    [switch] $Library,
    # User-owned dumps to include in the library without selecting an automatic boot game.
    [string[]] $LibraryGames = @(),
    # Local untracked selection/JIT records and guest shader environments.
    # Copied to a separate baseline; existing console caches remain authoritative.
    [string] $AstcCacheSeed,
    # Private captured draw inputs for the isolated replay, never added to the repository.
    [string] $AstcDrawReplaySeed,
    # Extra boot.cfg lines for diagnosis: "log_filter=*:Info HW.GPU:Debug", "renderer=null".
    [string[]] $BootCfg = @(),
    # Must match Identity/@Publisher in dist/uwp/AppxManifest.xml, character for character.
    [string] $PublisherCN = "CN=EdenXboxDev",
    # Mesa's SPIR-V -> DXIL translator for the D3D12 renderer, built by build-spirv-to-dxil.ps1.
    # Without it the renderer still presents, through its CPU fallback.
    [string] $SpirvToDxil = "..\mesa-build\build-uwp\src\microsoft\spirv_to_dxil\spirv_to_dxil.dll",
    [string] $OutDir = "build-uwp\series"
)

$ErrorActionPreference = "Stop"
if ($Library) { $BootCfg += 'library=1'; $BootCfg += 'play=1' }
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
$exe = Join-Path $repo "$BuildDir\bin\eden-uwp.exe"
if (-not (Test-Path $exe)) {
    $found = Get-ChildItem (Join-Path $repo $BuildDir) -Recurse -Filter eden-uwp.exe -ErrorAction SilentlyContinue |
             Select-Object -First 1
    if (-not $found) {
        throw "eden-uwp.exe not found under $BuildDir. Build it first from a 'vcvarsall.bat x64 uwp' shell:`n" +
              "  cmake --preset uwp-x64`n  cmake --build --preset uwp-x64 --target eden-uwp"
    }
    $exe = $found.FullName
}
Write-Host "exe      : $exe"

# --- 2. stage the package layout ---------------------------------------------------------
$layout = Join-Path $repo "$OutDir\layout"
$taskOutputRoot = [IO.Path]::GetFullPath((Join-Path $repo $OutDir))
$layout = [IO.Path]::GetFullPath($layout)
if (-not $layout.StartsWith($taskOutputRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Package layout escapes the requested output directory.'
}
if (Test-Path $layout) {
    if ((Get-Item -LiteralPath $layout).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw 'Package layout is a filesystem link; refusing recursive cleanup.'
    }
    Remove-Item -LiteralPath $layout -Recurse -Force
}
New-Item -ItemType Directory -Path $layout -Force | Out-Null

Copy-Item $exe $layout
Copy-Item (Join-Path $repo "dist\uwp\AppxManifest.xml") $layout
Copy-Item (Join-Path $repo "dist\uwp\Assets") $layout -Recurse
# Use Eden's existing artwork in the launcher, preserving the upstream asset.
Copy-Item -LiteralPath (Join-Path $repo 'dist\qt_themes\default\icons\256x256\eden.png') `
    -Destination (Join-Path $layout 'Assets\EdenLogo.png')

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
        Write-Warning "dxil.dll not found in the Windows SDK: the D3D12 renderer will present through the CPU."
    }
} else {
    Write-Warning "spirv_to_dxil.dll not found ($s2d): the D3D12 renderer will present through the CPU."
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
foreach ($libraryGame in $LibraryGames) {
    if (-not (Test-Path -LiteralPath $libraryGame -PathType Leaf)) {
        throw "Library game not found: $libraryGame"
    }
    $libraryGameName = Split-Path -Leaf $libraryGame
    New-Item -ItemType Directory -Force "$userdata\games" | Out-Null
    Copy-Item -LiteralPath $libraryGame -Destination (Join-Path "$userdata\games" $libraryGameName)
    Write-Host "library  : $libraryGame"
}
if ($AstcCacheSeed) {
    $seedRoot = (Resolve-Path -LiteralPath $AstcCacheSeed).Path
    foreach ($kind in @('shader','jit-profile')) {
        $kindRoot = Join-Path $seedRoot $kind
        if (-not (Test-Path -LiteralPath $kindRoot)) { continue }
        foreach ($title in Get-ChildItem -LiteralPath $kindRoot -Directory) {
            if ($title.Name -cnotmatch '^[0-9a-f]{16}$') { throw 'Invalid cache seed title id.' }
            $destination = Join-Path $userdata ('astc-seed-v1\'+$kind+'\'+$title.Name)
            New-Item -ItemType Directory -Force $destination | Out-Null
            $allowed = if ($kind -eq 'shader') {
                @('d3d12.bin','astc_guest_warm.txt','astc_variants_v1.bin')
            } else { @('core-0.bin','core-1.bin','core-2.bin','core-3.bin') }
            foreach ($file in Get-ChildItem -LiteralPath $title.FullName -File) {
                if ($file.Name -notin $allowed) { continue }
                Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $destination $file.Name)
            }
        }
    }
    Write-Host "ASTC seed: separate baseline from $seedRoot (no ROMs or host code)"
}

if ($AstcDrawReplaySeed) {
    $replaySource = (Resolve-Path -LiteralPath $AstcDrawReplaySeed).Path
    $replayAssets = Join-Path $layout 'astc-replay'
    New-Item -ItemType Directory -Path $replayAssets -Force | Out-Null
    $replayLabels = @('key','stage0-dxil','stage4-dxil','metadata','viewports','scissors','push',
        'buffer0-kind4-slot0','buffer1-kind3-slot0','buffer2-kind3-slot1',
        'buffer3-kind2-slot0','buffer4-kind2-slot1','buffer5-kind2-slot2','buffer6-kind2-slot3',
        'buffer7-kind2-slot4','buffer8-kind2-slot5','buffer9-kind2-slot6','buffer10-kind6-slot0','raw-view3')
    $replayNames = @($replayLabels | ForEach-Object { "astc-capture-9112b4082b675746-draw3-$_.bin" })
    $replayNames += 'astc-capture-9112b4082b675746-draw2-buffer4-kind2-slot1.bin'
    foreach ($replayName in $replayNames) {
        $replayMatches = @(Get-ChildItem -LiteralPath $replaySource -File |
            Where-Object { $_.Name.EndsWith($replayName) })
        if ($replayMatches.Count -eq 0 -and $replayName.EndsWith('-raw-view3.bin')) {
            $replayMatches = @(Get-ChildItem -LiteralPath $replaySource -Filter '*-raw-atlas.bin' -File)
        }
        if ($replayMatches.Count -ne 1) { throw "Replay input missing or ambiguous: $replayName" }
        Copy-Item -LiteralPath $replayMatches[0].FullName -Destination (Join-Path $replayAssets $replayName)
    }
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
} else {
    Write-Warning "No -BootNro given. The app will activate, fail to load the NRO and log status 2 to eden_uwp_diag.txt."
}

# Publisher mismatch between manifest and cert is the #1 sideload rejection; fail loudly here.
[xml]$mf = Get-Content (Join-Path $layout "AppxManifest.xml") -Raw
if ($mf.Package.Identity.Publisher -ne $PublisherCN) {
    throw "Publisher mismatch: manifest has '$($mf.Package.Identity.Publisher)' but signing with '$PublisherCN'."
}

# --- 3. pack -----------------------------------------------------------------------------
$makeappx = Find-SdkTool "MakeAppx.exe"
$appx = Join-Path $repo "$OutDir\eden-xbox.appx"
if (Test-Path $appx) { Remove-Item $appx -Force }
# A bundled game dump is gigabytes of encrypted data that does not compress: skip compression.
$packArgs = @("pack", "/d", $layout, "/p", $appx, "/o")
if (Test-Path "$userdata\games") { $packArgs += "/nc" }
& $makeappx @packArgs
if ($LASTEXITCODE -ne 0) { throw "MakeAppx failed ($LASTEXITCODE)." }

# --- 4. sign -----------------------------------------------------------------------------
# Reuse a matching cert if one is already in the user store, else mint a self-signed one.
$cert = Get-ChildItem Cert:\CurrentUser\My |
        Where-Object { $_.Subject -eq $PublisherCN -and $_.NotAfter -gt (Get-Date) } |
        Select-Object -First 1
if (-not $cert) {
    Write-Host "minting a self-signed code-signing cert for $PublisherCN"
    $cert = New-SelfSignedCertificate -Type Custom -Subject $PublisherCN `
        -KeyUsage DigitalSignature -FriendlyName "Eden Xbox sideload" `
        -CertStoreLocation "Cert:\CurrentUser\My" `
        -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3", "2.5.29.19={text}")
}

$signtool = Find-SdkTool "SignTool.exe"
& $signtool sign /fd SHA256 /sha1 $cert.Thumbprint /t http://timestamp.digicert.com $appx
if ($LASTEXITCODE -ne 0) { throw "SignTool failed ($LASTEXITCODE)." }

# The console must trust the signer: upload this .cer alongside the appx in the Device Portal.
$cer = Join-Path $repo "$OutDir\eden-xbox.cer"
Export-Certificate -Cert $cert -FilePath $cer -Type CERT | Out-Null

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
    Write-Warning ("Microsoft.VCLibs.x64.14.00.appx not found. The app will FAIL TO ACTIVATE on-console " +
                   "without it. Install the VS component Microsoft.VisualStudio.ComponentGroup.UWP.VC.")
}

Write-Host ""
if ($OutDir -eq 'build-uwp\series' -or $OutDir -eq 'build-uwp/series') {
    Copy-Item -LiteralPath $exe -Destination $taskOutputRoot -Force
    $pdb = [IO.Path]::ChangeExtension($exe, '.pdb')
    if (Test-Path -LiteralPath $pdb) {
        Copy-Item -LiteralPath $pdb -Destination $taskOutputRoot -Force
    }
    [IO.File]::WriteAllText((Join-Path $taskOutputRoot 'version.txt'),
                          "$($mf.Package.Identity.Version)`n")
    # The signed APPX is the Series deliverable; staging is only needed by local-run.
    Remove-Item -LiteralPath $layout -Recurse -Force
}
Write-Host "package  : $appx"
Write-Host "cert     : $cer"
if ($vclibs) { Write-Host "framework: $vcOut  (upload as a dependency package)" }
Write-Host "Next: Device Portal https://<xbox-ip>:11443 -> Add -> upload all of the above -> set the app to Game mode."
