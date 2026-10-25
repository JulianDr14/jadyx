# SPDX-FileCopyrightText: Copyright 2026 Jadyx Project
# SPDX-License-Identifier: GPL-3.0-or-later
# Run on a different PC in the same LAN while Jadyx is open in its library.
param([Parameter(Mandatory=$true)][uri]$Address)
$ErrorActionPreference = 'Stop'
if ($Address.Scheme -ne 'http' -or $Address.AbsolutePath -ne '/' -or $Address.Query -or $Address.UserInfo -or $Address.Fragment) {
    throw 'Use the HTTP address shown by Jadyx, with no path, query or credentials.'
}
$portalOrigin = $Address.GetLeftPart([System.UriPartial]::Authority)
$portalSecret = Read-Host 'Enter the 4-digit code shown in Jadyx' -AsSecureString
$portalCode = [System.Net.NetworkCredential]::new('', $portalSecret).Password
$portalSession = $null
try {
    if ($portalCode -notmatch '^\d{4}$') { throw 'Expected four digits.' }
    $portalPage = Invoke-WebRequest -UseBasicParsing -Uri "$portalOrigin/" -TimeoutSec 5
    if ($portalPage.StatusCode -ne 200) { throw 'Landing page did not return HTTP 200.' }
    $portalPair = Invoke-RestMethod -Uri "$portalOrigin/api/v1/pair" -Method Post -Headers @{Origin=$portalOrigin} `
        -ContentType 'application/json; charset=utf-8' -Body (@{code=$portalCode} | ConvertTo-Json -Compress) -TimeoutSec 5
    $portalSession = $portalPair.session
    $portalPair = $null
    $portalStatus = Invoke-RestMethod -Uri "$portalOrigin/api/v1/status" -Headers @{Authorization="Bearer $portalSession"} -TimeoutSec 5
    if (!$portalStatus.running -or $portalStatus.stage -notin @('connection-test','nsp')) { throw 'Unexpected server state.' }
    Write-Host 'PASS: remote LAN HTTP page, pairing and authenticated status. No files were uploaded or installed.'
    Read-Host 'Stop the server in Jadyx, then press Enter to check closure' | Out-Null
    $portalClosed = $false
    try {
        Invoke-WebRequest -UseBasicParsing -Uri "$portalOrigin/" -TimeoutSec 5 | Out-Null
    } catch {
        # An HTTP error still means a server answered. Closure needs a transport failure.
        if ($_.Exception.Response) { throw 'A server still answers at this address.' }
        $portalClosed = $true
    }
    if (!$portalClosed) { throw 'The server is still reachable.' }
    Write-Host 'PASS: listener closed. Also verify shutdown when launching a game and suspending the app.'
} finally {
    $portalCode = $null; $portalSession = $null; $portalSecret.Dispose()
}
