<#
  One-time PHONE_CAMERA setup (Windows 10 1809+ or 11). Run from an elevated PowerShell, or just run it and
  accept the UAC prompt:
    powershell -ExecutionPolicy Bypass -File install.ps1

  1. Installs the UsbDk USB filter driver (Red Hat, signed), unless -SkipUsbDk is supplied. It lets PHONE_CAMERA switch your phone into
     accessory mode while Windows' own phone (MTP) driver is attached.
  2. Copies PHONE_CAMERA to C:\Program Files\PHONE_CAMERA and registers the virtual camera with Windows: the Frame
     Server backend (MyCamVCam.dll) on Windows 11, or the DirectShow fallback (MyCamVCamDShow.dll) on
     Windows 10 — see docs/WINDOWS10-SUPPORT.md.
  3. Starts the companion and makes it start with Windows.
#>
param(
    [string]$BuildDir = "$PSScriptRoot\build\Release",
    [Alias('SkipUsbDriver')][switch]$SkipUsbDk,
    [switch]$StartWithWindows,
    [switch]$ElevatedChild
)
$ErrorActionPreference = 'Stop'

function Configure-UserSession([string]$InstallDirectory, [string]$Executable, [switch]$StartWithWindows) {
    $executablePath = Join-Path -Path $InstallDirectory -ChildPath $Executable
    if ($StartWithWindows) {
        Set-ItemProperty -Path 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'PHONE_CAMERA' `
            -Value ('"' + $executablePath + '"')
    }
    # Launch un-elevated in the user's session via Explorer.
    Start-Process -FilePath explorer.exe -ArgumentList ('"' + $executablePath + '"')
    Write-Host ''
    Write-Host 'PHONE_CAMERA is installed. Look for the camera icon in the system tray, then plug in your phone.' -ForegroundColor Green
    Write-Host 'The first time, your phone asks to open PHONE_CAMERA: tick "Always" and tap OK.'
}

function Invoke-FirewallCommand([string[]]$Arguments, [bool]$Required) {
    & netsh.exe @Arguments | Out-Null
    $exitCode = $LASTEXITCODE
    if ($exitCode -ne 0) {
        $message = "PHONE_CAMERA firewall command failed (exit $exitCode): netsh $($Arguments -join ' ')"
        if ($Required) { throw $message }
        Write-Warning $message
        return $false
    }
    return $true
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $elevatedArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`" -BuildDir `"$BuildDir`""
    if ($SkipUsbDk) { $elevatedArgs += ' -SkipUsbDk' }
    if ($StartWithWindows) { $elevatedArgs += ' -StartWithWindows' }
    $elevatedArgs += ' -ElevatedChild'
    $elevated = Start-Process powershell -Verb RunAs -ArgumentList $elevatedArgs -Wait -PassThru
    if ($elevated.ExitCode -ne 0) { throw "Elevated PHONE_CAMERA installation failed (exit $($elevated.ExitCode))." }
    Configure-UserSession "$env:ProgramFiles\PHONE_CAMERA" 'PHONE_CAMERA.exe' -StartWithWindows:$StartWithWindows
    return
}

if ([Environment]::OSVersion.Version.Build -lt 17763) { throw 'PHONE_CAMERA needs Windows 10 1809 or later.' }
$isWindows11 = [Environment]::OSVersion.Version.Build -ge 22000
$vcamDll = if ($isWindows11) { 'MyCamVCam.dll' } else { 'MyCamVCamDShow.dll' }
foreach ($f in 'PHONE_CAMERA.exe', 'PHONE_CAMERA_libusb.dll', 'MyCamVCam.dll', 'MyCamVCamDShow.dll') {
    if (-not (Test-Path "$BuildDir\$f")) { throw "$BuildDir\$f not found. Build first: cmake --build build --config Release" }
}

# --- 1. UsbDk -------------------------------------------------------------------------------------
$usbdkUrl = 'https://github.com/daynix/UsbDk/releases/download/v1.00-22/UsbDk_1.0.22_x64.msi'
$usbdkSha256 = '91F6F695E1E13C656024E6D3B55620BF08D8835EF05EE0496935BA6BB62466A5'
if (-not $SkipUsbDk -and -not (Test-Path "$env:WINDIR\System32\UsbDkHelper.dll")) {
    Write-Host 'Installing UsbDk driver...'
    $msi = $null
    $reserved = $false
    try {
        for ($attempt = 0; $attempt -lt 8 -and -not $reserved; $attempt++) {
            $msi = Join-Path $env:TEMP ("PHONE_CAMERA-UsbDk-$([guid]::NewGuid()).msi")
            try {
                New-Item -ItemType File -Path $msi -ErrorAction Stop | Out-Null
                $reserved = $true
            } catch {
                $reserved = $false
            }
        }
        if (-not $reserved) { throw 'Could not reserve a private temporary path for the UsbDk download.' }
        Invoke-WebRequest -Uri $usbdkUrl -OutFile $msi -UseBasicParsing
        if ((Get-FileHash -LiteralPath $msi -Algorithm SHA256).Hash -ne $usbdkSha256) {
            throw 'UsbDk download hash mismatch.'
        }
        $p = Start-Process msiexec.exe -ArgumentList "/i `"$msi`" /qn /norestart" -Wait -PassThru
        if ($p.ExitCode -notin 0, 3010) { throw "UsbDk install failed (msiexec exit $($p.ExitCode))." }
        if ($p.ExitCode -eq 3010) { Write-Warning 'UsbDk asks for a reboot before it works.' }
    } finally {
        if ($reserved -and $msi -and (Test-Path -LiteralPath $msi)) {
            Remove-Item -LiteralPath $msi -Force -ErrorAction SilentlyContinue
        }
    }
} elseif (-not $SkipUsbDk) {
    Write-Host 'UsbDk already installed.'
} else {
    Write-Host 'Skipping UsbDk: Wi-Fi and iPhone (Safari) do not need the Android USB driver.'
}

# --- 2. Files + virtual camera registration ------------------------------------------------------
$dest = "$env:ProgramFiles\PHONE_CAMERA"
$exe = 'PHONE_CAMERA.exe'
New-Item -ItemType Directory -Force $dest | Out-Null
# Ask only our own companion to leave cleanly. Never terminate MyCam or another process, and never stop
# Windows' Frame Server service. If a camera host still holds a file, the copy below reports that the user
# must close the app and retry.
if (Test-Path "$dest\$exe") {
    & "$dest\$exe" --quit | Out-Null
}
# Unregister whichever backend a previous install left (DllUnregisterServer on the inactive one is a
# harmless no-op), so the files can be replaced.
foreach ($dll in 'MyCamVCam.dll', 'MyCamVCamDShow.dll') {
    if (Test-Path "$dest\$dll") {
        $r = Start-Process regsvr32.exe -ArgumentList "/s /u `"$dest\$dll`"" -Wait -PassThru
        if ($r.ExitCode -ne 0) { Write-Warning "Could not unregister $dll; close apps using PHONE_CAMERA and retry if replacement fails." }
    }
}
Copy-Item -LiteralPath (Join-Path -Path $BuildDir -ChildPath $exe) -Destination (Join-Path -Path $dest -ChildPath $exe) -Force
Copy-Item -LiteralPath (Join-Path -Path $BuildDir -ChildPath 'PHONE_CAMERA_libusb.dll') -Destination (Join-Path -Path $dest -ChildPath 'PHONE_CAMERA_libusb.dll') -Force
Copy-Item -LiteralPath (Join-Path -Path $BuildDir -ChildPath 'MyCamVCam.dll') -Destination (Join-Path -Path $dest -ChildPath 'MyCamVCam.dll') -Force
Copy-Item -LiteralPath (Join-Path -Path $BuildDir -ChildPath 'MyCamVCamDShow.dll') -Destination (Join-Path -Path $dest -ChildPath 'MyCamVCamDShow.dll') -Force
$r = Start-Process regsvr32.exe -ArgumentList "/s `"$dest\$vcamDll`"" -Wait -PassThru
if ($r.ExitCode -ne 0) { throw "Registering $vcamDll failed (regsvr32 exit $($r.ExitCode))." }
# Scheduled task that sets up new phones' USB driver without a UAC prompt.
if (-not $SkipUsbDk) {
    $r = Start-Process "$dest\$exe" -ArgumentList '--register-task' -Wait -PassThru
    if ($r.ExitCode -ne 0) { Write-Warning 'Could not register the phone driver task; new phones will ask for admin once.' }
} else {
    Write-Host 'Skipping the PHONE_CAMERA USB driver task.'
}
# The iPhone page (TCP 47900): allowed in on private networks only, as the installer does.
Invoke-FirewallCommand @('advfirewall', 'firewall', 'delete', 'rule', 'name=PHONE_CAMERA iPhone camera') $false
Invoke-FirewallCommand @('advfirewall', 'firewall', 'add', 'rule', 'name=PHONE_CAMERA iPhone camera', 'dir=in',
    'action=allow', "program=`"$dest\$exe`"", 'protocol=TCP', 'localport=47900', 'profile=private', 'enable=yes') $true

# --- 3. Autostart + launch ------------------------------------------------------------------------
if ($ElevatedChild) {
    Write-Host 'Machine-wide PHONE_CAMERA installation completed; returning to the original user session for autostart and launch.'
    return
}
Configure-UserSession $dest $exe -StartWithWindows:$StartWithWindows
