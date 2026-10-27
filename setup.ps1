param(
  [switch]$BuildOnly,
  [switch]$SkipUsbDk,
  [switch]$StartWithWindows,
  [string]$BuildDir = ''
)

$ErrorActionPreference = 'Stop'
$nativePc = Join-Path $PSScriptRoot 'native\phone-camera\pc'
if (-not $BuildDir) { $BuildDir = Join-Path $nativePc 'build' }

& (Join-Path $PSScriptRoot 'scripts\build-windows.ps1') -BuildDir $BuildDir
if ($BuildOnly) {
  Write-Host 'Build complete. No driver, firewall, startup or camera registration changes were made.'
  return
}

Write-Host 'Installing PHONE_CAMERA and registering its Windows virtual camera.'
if ($SkipUsbDk) {
  Write-Host 'Skipping the Android USB driver; iPhone and Wi-Fi modes do not require it.'
}
& (Join-Path $nativePc 'install.ps1') -BuildDir (Join-Path $BuildDir 'Release') -SkipUsbDk:$SkipUsbDk -StartWithWindows:$StartWithWindows
