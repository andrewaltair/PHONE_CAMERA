<#
  Removes MyCam. Pass -RemoveUsbDk to also uninstall the UsbDk driver (other apps, e.g. virt-viewer, may use it).
#>
param([switch]$RemoveUsbDk)
$ErrorActionPreference = 'Stop'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $extra = if ($RemoveUsbDk) { ' -RemoveUsbDk' } else { '' }
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`"$extra"
    return
}

$dest = "$env:ProgramFiles\MyCam"
Get-Process MyCamCompanion -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'MyCam' -ErrorAction SilentlyContinue
Remove-Item 'HKCU:\Software\MyCam' -Recurse -ErrorAction SilentlyContinue
foreach ($dll in 'MyCamVCam.dll', 'MyCamVCamDShow.dll') {
    if (Test-Path "$dest\$dll") { & regsvr32.exe /s /u "$dest\$dll" }
}
& schtasks.exe /Delete /TN 'MyCam phone driver' /F 2>$null | Out-Null
& netsh.exe advfirewall firewall delete rule name="MyCam iPhone camera" 2>$null | Out-Null
Restart-Service FrameServer -Force -ErrorAction SilentlyContinue
Remove-Item $dest -Recurse -Force -ErrorAction SilentlyContinue

if ($RemoveUsbDk) {
    $pkg = Get-Package -Name 'UsbDk*' -ErrorAction SilentlyContinue
    if ($pkg) { $pkg | Uninstall-Package -Force | Out-Null }
}
Write-Host 'MyCam removed.' -ForegroundColor Green
