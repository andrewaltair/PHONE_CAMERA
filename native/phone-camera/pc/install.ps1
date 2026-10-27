<#
  One-time MyCam setup (Windows 10 1809+ or 11). Run from an elevated PowerShell, or just run it and
  accept the UAC prompt:
    powershell -ExecutionPolicy Bypass -File install.ps1

  1. Installs the UsbDk USB filter driver (Red Hat, signed). It lets MyCam switch your phone into
     accessory mode while Windows' own phone (MTP) driver is attached.
  2. Copies MyCam to C:\Program Files\MyCam and registers the virtual camera with Windows: the Frame
     Server backend (MyCamVCam.dll) on Windows 11, or the DirectShow fallback (MyCamVCamDShow.dll) on
     Windows 10 — see docs/WINDOWS10-SUPPORT.md.
  3. Starts the companion and makes it start with Windows.
#>
param([string]$BuildDir = "$PSScriptRoot\build\Release")
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -BuildDir `"$BuildDir`""
    return
}

if ([Environment]::OSVersion.Version.Build -lt 17763) { throw 'MyCam needs Windows 10 1809 or later.' }
$isWindows11 = [Environment]::OSVersion.Version.Build -ge 22000
$vcamDll = if ($isWindows11) { 'MyCamVCam.dll' } else { 'MyCamVCamDShow.dll' }
foreach ($f in 'MyCamCompanion.exe', $vcamDll) {
    if (-not (Test-Path "$BuildDir\$f")) { throw "$BuildDir\$f not found. Build first: cmake --build build --config Release" }
}

# --- 1. UsbDk -------------------------------------------------------------------------------------
$usbdkUrl = 'https://github.com/daynix/UsbDk/releases/download/v1.00-22/UsbDk_1.0.22_x64.msi'
$usbdkSha256 = '91F6F695E1E13C656024E6D3B55620BF08D8835EF05EE0496935BA6BB62466A5'
if (-not (Test-Path "$env:WINDIR\System32\UsbDkHelper.dll")) {
    Write-Host 'Installing UsbDk driver...'
    $msi = Join-Path $env:TEMP 'UsbDk_1.0.22_x64.msi'
    Invoke-WebRequest -Uri $usbdkUrl -OutFile $msi -UseBasicParsing
    if ((Get-FileHash $msi -Algorithm SHA256).Hash -ne $usbdkSha256) { Remove-Item $msi; throw 'UsbDk download hash mismatch.' }
    $p = Start-Process msiexec.exe -ArgumentList "/i `"$msi`" /qn /norestart" -Wait -PassThru
    Remove-Item $msi
    if ($p.ExitCode -notin 0, 3010) { throw "UsbDk install failed (msiexec exit $($p.ExitCode))." }
    if ($p.ExitCode -eq 3010) { Write-Warning 'UsbDk asks for a reboot before it works.' }
} else {
    Write-Host 'UsbDk already installed.'
}

# --- 2. Files + virtual camera registration ------------------------------------------------------
$dest = "$env:ProgramFiles\MyCam"
Get-Process MyCamCompanion -ErrorAction SilentlyContinue | Stop-Process -Force
New-Item -ItemType Directory -Force $dest | Out-Null
# Unregister whichever backend a previous install left (DllUnregisterServer on the inactive one is a
# harmless no-op), so the files can be replaced.
foreach ($dll in 'MyCamVCam.dll', 'MyCamVCamDShow.dll') {
    if (Test-Path "$dest\$dll") { & regsvr32.exe /s /u "$dest\$dll" }
}
if ($isWindows11) { Restart-Service FrameServer -Force -ErrorAction SilentlyContinue }
Copy-Item "$BuildDir\MyCamCompanion.exe", "$BuildDir\$vcamDll" $dest -Force
$r = Start-Process regsvr32.exe -ArgumentList "/s `"$dest\$vcamDll`"" -Wait -PassThru
if ($r.ExitCode -ne 0) { throw "Registering $vcamDll failed (regsvr32 exit $($r.ExitCode))." }
# Scheduled task that sets up new phones' USB driver without a UAC prompt.
$r = Start-Process "$dest\MyCamCompanion.exe" -ArgumentList '--register-task' -Wait -PassThru
if ($r.ExitCode -ne 0) { Write-Warning 'Could not register the phone driver task; new phones will ask for admin once.' }
# The iPhone page (TCP 47900): allowed in on private networks only, as the installer does.
& netsh.exe advfirewall firewall delete rule name="MyCam iPhone camera" | Out-Null
& netsh.exe advfirewall firewall add rule name="MyCam iPhone camera" dir=in action=allow program="$dest\MyCamCompanion.exe" protocol=TCP localport=47900 profile=private enable=yes | Out-Null

# --- 3. Autostart + launch ------------------------------------------------------------------------
Set-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'MyCam' -Value "`"$dest\MyCamCompanion.exe`""
# Launch un-elevated in the user's session via Explorer.
Start-Process explorer.exe "`"$dest\MyCamCompanion.exe`""

Write-Host ''
Write-Host 'MyCam is installed. Look for the camera icon in the system tray, then plug in your phone.' -ForegroundColor Green
Write-Host 'The first time, your phone asks to open MyCam: tick "Always" and tap OK.'
