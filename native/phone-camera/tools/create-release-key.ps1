<#
  Creates MyCam's Android release signing key, once. Run it yourself, in a PowerShell window:
    powershell -ExecutionPolicy Bypass -File tools\create-release-key.ps1

  - The keystore goes to %USERPROFILE%\.mycam\mycam-release.jks (outside the repo).
  - You type the password; it is passed to keytool through an environment variable, never on a command line.
  - keystore.properties (git-ignored) is written in the repo root so Gradle can sign release builds.

  BACK UP the .jks file and the password somewhere safe (password manager + a copy of the file).
  If either is lost, future versions can't be installed over existing copies of the app.
#>
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\.."
$keyDir = Join-Path $env:USERPROFILE '.mycam'
$keystore = Join-Path $keyDir 'mycam-release.jks'
$alias = 'mycam'

if (Test-Path $keystore) { throw "A release key already exists at $keystore. Not overwriting it." }

$keytool = @("$env:JAVA_HOME\bin\keytool.exe", 'C:\Program Files\Android\Android Studio\jbr\bin\keytool.exe') |
    Where-Object { $_ -and (Test-Path $_) } | Select-Object -First 1
if (-not $keytool) { throw 'keytool not found. Install Android Studio or set JAVA_HOME.' }

function Read-Secret([string]$prompt) {
    $secure = Read-Host -Prompt $prompt -AsSecureString
    $ptr = [Runtime.InteropServices.Marshal]::SecureStringToBSTR($secure)
    try { [Runtime.InteropServices.Marshal]::PtrToStringBSTR($ptr) } finally { [Runtime.InteropServices.Marshal]::ZeroFreeBSTR($ptr) }
}

$pass = Read-Secret 'Choose a password for the MyCam release key (at least 8 characters)'
if ($pass.Length -lt 8) { throw 'Password too short (minimum 8 characters).' }
if ($pass -ne (Read-Secret 'Type it again')) { throw 'Passwords did not match.' }

New-Item -ItemType Directory -Force $keyDir | Out-Null
$env:MYCAM_KS_PASS = $pass
try {
    & $keytool -genkeypair -keystore $keystore -storetype PKCS12 -alias $alias -keyalg RSA -keysize 4096 `
        -validity 10000 -dname 'CN=whoissaaif, O=MyCam' -storepass:env MYCAM_KS_PASS -keypass:env MYCAM_KS_PASS
    if ($LASTEXITCODE) { throw 'keytool failed.' }

    # Gradle reads this (git-ignored). Forward slashes keep the Java properties format happy.
    $props = @(
        "storeFile=$($keystore.Replace('\', '/'))"
        "storePassword=$pass"
        "keyAlias=$alias"
        "keyPassword=$pass"
    )
    Set-Content -Path (Join-Path $root 'keystore.properties') -Value $props -Encoding ascii

    Write-Host ''
    Write-Host "Release key created: $keystore" -ForegroundColor Green
    & $keytool -list -keystore $keystore -alias $alias -storepass:env MYCAM_KS_PASS | Select-String 'SHA-256|SHA256'
} finally {
    Remove-Item Env:\MYCAM_KS_PASS -ErrorAction SilentlyContinue
}
Write-Host ''
Write-Host 'NOW BACK UP the .jks file and the password (password manager + a second copy of the file).' -ForegroundColor Yellow
