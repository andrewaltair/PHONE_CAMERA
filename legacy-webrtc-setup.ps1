# iphone-stream — one-command setup + run.
#   Right-click > Run with PowerShell, or:  powershell -ExecutionPolicy Bypass -File setup.ps1
# Detects your LAN/USB IP, makes a TRUSTED cert (mkcert) for it, then starts the server.

$ErrorActionPreference = 'Stop'
$dir = $PSScriptRoot
$mkcert = Join-Path $dir 'mkcert.exe'

Write-Host "`n=== iphone-stream setup ===" -ForegroundColor Cyan

# 1. mkcert binary (download once, no admin needed)
if (-not (Test-Path $mkcert)) {
  Write-Host "downloading mkcert..."
  [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
  Invoke-WebRequest -UseBasicParsing `
    -Uri "https://github.com/FiloSottile/mkcert/releases/download/v1.4.4/mkcert-v1.4.4-windows-amd64.exe" `
    -OutFile $mkcert
}

# 2. install the local CA into the Windows trust store (idempotent; may show one "install certificate?" dialog -> Yes)
& $mkcert -install

# 3. detect usable IPv4s (skip virtual adapters), rank USB > 192.168 > 10 > rest
$ips = Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
  Where-Object {
    $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' -and
    $_.InterfaceAlias -notmatch 'vEthernet|WSL|Loopback|Hyper-V|VMware|VirtualBox|Default Switch'
  } | Select-Object -ExpandProperty IPAddress -Unique

function Rank($ip) {
  if ($ip -like '172.20.10.*') { 0 } elseif ($ip -like '192.168.*') { 1 } elseif ($ip -like '10.*') { 2 } else { 3 }
}
$ips = @($ips | Sort-Object { Rank $_ })
if ($ips.Count -eq 0) { Write-Host "No LAN IPv4 found — connect Wi-Fi or USB tether first." -ForegroundColor Red; exit 1 }
Write-Host ("detected IPs: " + ($ips -join ', ')) -ForegroundColor Green

# 4. trusted cert for every detected IP + localhost, and the rootCA for the iPhone
New-Item -ItemType Directory -Force -Path (Join-Path $dir 'cert') | Out-Null
$sans = $ips + @('localhost', '127.0.0.1')
& $mkcert -cert-file (Join-Path $dir 'cert\cert.pem') -key-file (Join-Path $dir 'cert\key.pem') @sans
Copy-Item (Join-Path (& $mkcert -CAROOT) 'rootCA.pem') (Join-Path $dir 'cert\rootCA.pem') -Force

# 5. node deps
if (-not (Test-Path (Join-Path $dir 'node_modules\ws'))) {
  Write-Host "installing node deps..."
  npm install --prefix $dir | Out-Null
}

# 6. firewall rule for the port (best-effort; ignore if no rights)
try {
  if (-not (Get-NetFirewallRule -DisplayName 'iphone-stream 8443' -ErrorAction SilentlyContinue)) {
    New-NetFirewallRule -DisplayName 'iphone-stream 8443' -Direction Inbound -Action Allow `
      -Protocol TCP -LocalPort 8443 -Profile Private -ErrorAction Stop | Out-Null
    Write-Host "firewall: allowed inbound 8443 (private)" -ForegroundColor Green
  }
} catch { Write-Host "firewall: skipped (need admin) — accept the Windows prompt on first run" -ForegroundColor Yellow }

Write-Host "`nPhone (iPhone, same Wi-Fi or USB):" -ForegroundColor Cyan
foreach ($ip in $ips) { Write-Host ("  https://{0}:8443/phone.html" -f $ip) }
Write-Host "PC viewer:  https://localhost:8443/viewer.html" -ForegroundColor Cyan
Write-Host "iPhone first time -> open /rootCA.pem, install profile, Settings > General > About > Certificate Trust Settings > ON`n" -ForegroundColor Yellow

# 7. run (foreground; Ctrl+C to stop). Prints a QR of the phone URL to scan.
node (Join-Path $dir 'server.js')
