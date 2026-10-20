# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Signs exact release metadata bytes; never exports a private key.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$Package,
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet('stable','preview')][string]$Channel='stable',
    [string]$Notes='',
    [string]$PfxPath,
    [string]$CertificateThumbprint='82FAA23764FECD4E03DF6F379E2504A859414B49'
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Security.Cryptography.Pkcs
$null=& "$PSScriptRoot/verify-appx.ps1" -Package $Package
$archive=[IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Package).Path)
try {
    $entry=$archive.GetEntry('AppxManifest.xml')
    if(-not $entry -or $entry.Length -gt 65536){throw 'Missing or oversized package manifest'}
    $reader=[IO.StreamReader]::new($entry.Open())
    try {[xml]$manifest=$reader.ReadToEnd()}finally{$reader.Dispose()}
    $id=$manifest.Package.Identity
    if($id.Name -ne 'Jadyx' -or $id.Publisher -ne 'CN=JadyxDev' -or $id.ProcessorArchitecture -ne 'x64'){throw 'Wrong package identity'}
    $version=[string]$id.Version
    if($version -notmatch '^(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)$' -or
       @($version.Split('.')|Where-Object {[int]$_ -gt 65535}).Count){throw 'Invalid package version'}
    foreach($item in $archive.Entries){
        if($item.FullName -match '(?i)(^|/)(prod.keys|title.keys|eden_log.txt|jadyx_diag.txt)$'){throw 'User payload found'}
        if($item.FullName -match '(?i)(^|/)boot.cfg$'){
            $reader=[IO.StreamReader]::new($item.Open())
            try{$cfg=$reader.ReadToEnd()}finally{$reader.Dispose()}
            if($cfg -match '(?im)^\s*[^#;\r\n]*(gate|capture|hunt|debug|profile)\w*\s*=\s*[1-9]'){throw 'Diagnostic package is not eligible'}
        }
    }
} finally {$archive.Dispose()}
if([Text.Encoding]::UTF8.GetByteCount($Notes) -gt 16384){throw 'Release notes exceed metadata limit'}
if($PfxPath){
    if(-not $env:JADYX_PFX_PASSWORD){throw 'JADYX_PFX_PASSWORD is missing'}
    $cert=[Security.Cryptography.X509Certificates.X509Certificate2]::new((Resolve-Path -LiteralPath $PfxPath).Path,$env:JADYX_PFX_PASSWORD,
        [Security.Cryptography.X509Certificates.X509KeyStorageFlags]::EphemeralKeySet)
}else{$cert=Get-Item -LiteralPath "Cert:\CurrentUser\My\$CertificateThumbprint"}
$rsa=[Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPrivateKey($cert)
if(-not $rsa){throw 'Release signing certificate has no RSA private key'}
try{
    $public=$rsa.ExportSubjectPublicKeyInfoPem() -replace '\s',''
    $pinned=(Get-Content -LiteralPath "$PSScriptRoot\..\..\src\jadyx\updates\release_public_key.pem" -Raw) -replace '\s',''
    if($public -cne $pinned){throw 'Signing key differs from the key trusted by Jadyx'}
    $file=Get-Item -LiteralPath $Package
    $metadata=[ordered]@{
        schema=1;version=$version;tag="v$version";channel=$Channel
        identity=@{name='Jadyx';publisher='CN=JadyxDev';architecture='x64'}
        min_os='10.0.17763.0';vclibs_min='14.0.33519.0';diagnostic=$false
        url="https://github.com/JulianDr14/jadyx/releases/download/v$version/Jadyx_${version}_x64.appx"
        size=$file.Length;sha256=(Get-FileHash -LiteralPath $Package -Algorithm SHA256).Hash.ToLowerInvariant()
        notes=$Notes;published=[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')
    }
    $bytes=[Text.UTF8Encoding]::new($false).GetBytes(($metadata|ConvertTo-Json -Depth 8 -Compress))
    $signature=$rsa.SignData($bytes,[Security.Cryptography.HashAlgorithmName]::SHA256,[Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $out=New-Item -ItemType Directory -Path $OutputDirectory -Force
    if((Test-Path -LiteralPath "$out/update.json") -or (Test-Path -LiteralPath "$out/update.sig")){throw 'Metadata already exists; assets must be immutable'}
    [IO.File]::WriteAllBytes((Join-Path $out 'update.json'),$bytes)
    [IO.File]::WriteAllBytes((Join-Path $out 'update.sig'),$signature)
}finally{$rsa.Dispose();$cert.Dispose()}
