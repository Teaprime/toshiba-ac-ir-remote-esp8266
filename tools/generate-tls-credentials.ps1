$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$webDirectory = Join-Path $repoRoot "src\web"
$credentialsPath = Join-Path $webDirectory "tls_credentials.h"
$caCertificatePath = Join-Path $webDirectory "toshiba-ac-ca.crt"
$commandSecretPath = Join-Path $webDirectory "command-secret.txt"

$opensslCommand = Get-Command openssl -ErrorAction SilentlyContinue
if ($opensslCommand) {
    $openssl = $opensslCommand.Source
} else {
    $openssl = Join-Path $env:ProgramFiles "Git\usr\bin\openssl.exe"
}
if (-not (Test-Path $openssl)) {
    throw "OpenSSL was not found. Install OpenSSL or Git for Windows, then rerun this script."
}

$temporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("toshiba-ac-tls-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $temporaryDirectory | Out-Null

try {
    $caKey = Join-Path $temporaryDirectory "ca.key"
    $caCertificate = Join-Path $temporaryDirectory "ca.crt"
    $serverKey = Join-Path $temporaryDirectory "server.key"
    $serverRequest = Join-Path $temporaryDirectory "server.csr"
    $serverCertificate = Join-Path $temporaryDirectory "server.crt"
    $extensions = Join-Path $temporaryDirectory "server.ext"
    $serial = Join-Path $temporaryDirectory "ca.srl"
    $commandSecretFile = Join-Path $temporaryDirectory "command-secret.txt"

    & $openssl req -x509 -newkey rsa:2048 -nodes -keyout $caKey -out $caCertificate `
        -days 3650 -sha256 -subj "/CN=Toshiba AC Local CA" `
        -addext "basicConstraints=critical,CA:TRUE" `
        -addext "keyUsage=critical,keyCertSign,cRLSign"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create the local CA certificate."
    }

    $commandSecret = (& $openssl rand -hex 32).Trim()
    if ($LASTEXITCODE -ne 0 -or $commandSecret.Length -ne 64) {
        throw "Failed to generate the shared command secret."
    }
    [System.IO.File]::WriteAllText($commandSecretFile, $commandSecret, [System.Text.Encoding]::ASCII)

    & $openssl req -new -newkey rsa:2048 -nodes -keyout $serverKey -out $serverRequest `
        -sha256 -subj "/CN=toshiba-ac.local"
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to create the HTTPS server key and certificate request."
    }

    @"
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:toshiba-ac.local
"@ | Set-Content -Path $extensions -Encoding ascii

    & $openssl x509 -req -in $serverRequest -CA $caCertificate -CAkey $caKey `
        -CAserial $serial -CAcreateserial -out $serverCertificate -days 3650 `
        -sha256 -extfile $extensions
    if ($LASTEXITCODE -ne 0) {
        throw "Failed to sign the HTTPS server certificate."
    }

    $certificateText = (Get-Content -Path $serverCertificate -Raw).Trim()
    $privateKeyText = (Get-Content -Path $serverKey -Raw).Trim()
    @"
#pragma once

static const char tlsServerCert[] PROGMEM = R"TLS_CERT(
$certificateText
)TLS_CERT";

static const char tlsServerKey[] PROGMEM = R"TLS_KEY(
$privateKeyText
)TLS_KEY";

static const char commandSecret[] PROGMEM = "$commandSecret";
"@ | Set-Content -Path $credentialsPath -Encoding ascii

    Copy-Item -Path $caCertificate -Destination $caCertificatePath -Force
    Copy-Item -Path $commandSecretFile -Destination $commandSecretPath -Force
    Write-Host "Created ignored server credentials: $credentialsPath"
    Write-Host "Install this CA certificate on trusted client devices: $caCertificatePath"
    Write-Host "Enter the generated shared secret from this ignored file in the controller: $commandSecretPath"
} finally {
    Remove-Item -LiteralPath $temporaryDirectory -Recurse -Force
}
