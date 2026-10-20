# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Bounded-memory verification of payload blocks and their signed metadata chain.
# Windows still performs its own package policy/deployment checks at installation.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Package)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.Security.Cryptography.Pkcs
$archive=[IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $Package).Path)
try {
    $names=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach($entry in $archive.Entries){
        if(-not $names.Add($entry.FullName) -or $entry.FullName -match '(^|/)\.\.?(/|$)' -or $entry.FullName.Contains('\')){throw 'Duplicate or unsafe APPX entry'}
    }
    function Read-Metadata([string]$Name,[long]$MaxBytes){
        $item=$archive.GetEntry($Name)
        if(-not $item -or $item.Length -gt $MaxBytes){throw "Missing or oversized $Name"}
        $stream=$item.Open();$memory=[IO.MemoryStream]::new()
        try{$stream.CopyTo($memory);return ,$memory.ToArray()}finally{$stream.Dispose();$memory.Dispose()}
    }
    $signature=Read-Metadata 'AppxSignature.p7x' 1048576
    if($signature.Length -lt 5 -or [Text.Encoding]::ASCII.GetString($signature,0,4) -ne 'PKCX'){throw 'Invalid APPX signature'}
    $cms=[Security.Cryptography.Pkcs.SignedCms]::new();$cms.Decode($signature[4..($signature.Length-1)]);$cms.CheckSignature($true)
    if($cms.SignerInfos.Count -ne 1 -or $cms.ContentInfo.ContentType.Value -ne '1.3.6.1.4.1.311.2.1.4'){throw 'Unexpected APPX signature content'}
    $public=[Security.Cryptography.X509Certificates.RSACertificateExtensions]::GetRSAPublicKey($cms.SignerInfos[0].Certificate)
    try{$pem=$public.ExportSubjectPublicKeyInfoPem() -replace '\s',''}finally{$public.Dispose()}
    $pin=(Get-Content -LiteralPath "$PSScriptRoot/../../src/jadyx/updates/release_public_key.pem" -Raw) -replace '\s',''
    if($pem -cne $pin){throw 'Unexpected APPX signer'}
    # SpcIndirectDataContent -> DigestInfo -> APPX digest table (Microsoft MSIX SDK).
    $asn=[System.Formats.Asn1.AsnReader]::new([ReadOnlyMemory[byte]]::new($cms.ContentInfo.Content),[System.Formats.Asn1.AsnEncodingRules]::DER)
    $outer=$asn.ReadSequence();$null=$outer.ReadEncodedValue()
    $digest=$outer.ReadSequence();$algorithm=$digest.ReadSequence()
    if($algorithm.ReadObjectIdentifier() -ne '2.16.840.1.101.3.4.2.1'){throw 'Expected SHA-256 APPX digests'}
    if($algorithm.HasData){$algorithm.ReadNull()};$algorithm.ThrowIfNotEmpty()
    $table=$digest.ReadOctetString();$digest.ThrowIfNotEmpty();$outer.ThrowIfNotEmpty();$asn.ThrowIfNotEmpty()
    if($table.Length -lt 4 -or ($table.Length-4)%36 -ne 0 -or [Text.Encoding]::ASCII.GetString($table,0,4) -ne 'APPX'){throw 'Malformed APPX digest table'}
    $digests=@{}
    for($offset=4;$offset -lt $table.Length;$offset+=36){
        $tag=[Text.Encoding]::ASCII.GetString($table,$offset,4)
        if($digests.ContainsKey($tag)){throw 'Duplicate signed digest'}
        $digests[$tag]=[Convert]::ToHexString($table[($offset+4)..($offset+35)])
    }
    if(-not $digests.ContainsKey('AXPC') -or -not $digests.ContainsKey('AXCD')){throw 'Missing signed ZIP digests'}
    $null=& python "$PSScriptRoot/verify-appx-structure.py" $Package $digests['AXPC'] $digests['AXCD']
    if($LASTEXITCODE -ne 0){throw 'Signed APPX container verification failed'}
    $blockBytes=Read-Metadata 'AppxBlockMap.xml' 8388608
    $contentBytes=Read-Metadata '[Content_Types].xml' 1048576
    foreach($pair in @(@('AXBM',$blockBytes),@('AXCT',$contentBytes))){
        if($digests[$pair[0]] -cne [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([byte[]]$pair[1]))){throw "Signed metadata digest mismatch: $($pair[0])"}
    }
    $integrity=$archive.GetEntry('AppxMetadata/CodeIntegrity.cat')
    if($integrity){
        $integrityBytes=Read-Metadata 'AppxMetadata/CodeIntegrity.cat' 8388608
        if($digests['AXCI'] -cne [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($integrityBytes))){throw 'Code integrity digest mismatch'}
    } elseif($digests.ContainsKey('AXCI')) {throw 'Signed code integrity file is missing'}
    $xmlSettings=[Xml.XmlReaderSettings]::new();$xmlSettings.DtdProcessing=[Xml.DtdProcessing]::Prohibit;$xmlSettings.XmlResolver=$null
    $xmlStream=[IO.MemoryStream]::new($blockBytes);$reader=[Xml.XmlReader]::Create($xmlStream,$xmlSettings)
    try{$blockMap=[Xml.XmlDocument]::new();$blockMap.XmlResolver=$null;$blockMap.Load($reader)}finally{$reader.Dispose();$xmlStream.Dispose()}
    if($blockMap.DocumentElement.NamespaceURI -ne 'http://schemas.microsoft.com/appx/2010/blockmap' -or $blockMap.BlockMap.HashMethod -ne 'http://www.w3.org/2001/04/xmlenc#sha256'){throw 'Unsupported block map'}
    $covered=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    $buffer=[byte[]]::new(65536);$blocks=0;$files=0
    foreach($file in $blockMap.BlockMap.File){
        $name=([string]$file.Name).Replace('\','/')
        if(-not $covered.Add($name)){throw 'Duplicate block map file'}
        $item=$archive.GetEntry($name)
        if(-not $item -or $item.Length -ne [long]$file.Size){throw "Block map size mismatch: $name"}
        $stream=$item.Open();$remaining=$item.Length
        try{
            foreach($block in $file.Block){
                $take=[int][Math]::Min(65536,$remaining);$got=0
                while($got -lt $take){$n=$stream.Read($buffer,$got,$take-$got);if($n -le 0){throw "Truncated block: $name"};$got+=$n}
                if($take -eq 0 -and $item.Length -ne 0){throw 'Extra block map entry'}
                $sha=[Security.Cryptography.SHA256]::Create()
                try{$actual=[Convert]::ToBase64String($sha.ComputeHash($buffer,0,$take))}finally{$sha.Dispose()}
                if($actual -cne [string]$block.Hash){throw "Payload block hash mismatch: $name"}
                $remaining-=$take;$blocks++
            }
            if($remaining -ne 0 -or $stream.ReadByte() -ne -1){throw "Incomplete block coverage: $name"}
        }finally{$stream.Dispose()}
        $files++
    }
    foreach($item in $archive.Entries){
        if($item.FullName -notin @('AppxSignature.p7x','AppxBlockMap.xml','[Content_Types].xml','AppxMetadata/CodeIntegrity.cat') -and -not $covered.Contains($item.FullName)){throw "Unsigned payload entry: $($item.FullName)"}
    }
    [pscustomobject]@{Package=(Resolve-Path -LiteralPath $Package).Path;Signer='JadyxDev pinned key';Files=$files;Blocks=$blocks;CMS='valid';SignedMetadata='valid';Payload='valid';SignedContainer='valid'}
}finally{$archive.Dispose()}
