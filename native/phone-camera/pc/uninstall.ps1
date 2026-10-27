<#
  Removes PHONE_CAMERA. Pass -RemoveUsbDk to also uninstall the UsbDk driver (other apps, e.g. virt-viewer, may use it).
#>
param([switch]$RemoveUsbDk, [switch]$ElevatedChild)
$ErrorActionPreference = 'Stop'

function Remove-UserSessionState {
    Remove-ItemProperty 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run' -Name 'PHONE_CAMERA' -ErrorAction SilentlyContinue
    Remove-Item 'HKCU:\Software\PHONE_CAMERA' -Recurse -ErrorAction SilentlyContinue
}

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    $elevatedArgs = "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    if ($RemoveUsbDk) { $elevatedArgs += ' -RemoveUsbDk' }
    $elevatedArgs += ' -ElevatedChild'
    $elevated = Start-Process powershell -Verb RunAs -ArgumentList $elevatedArgs -Wait -PassThru
    if ($elevated.ExitCode -notin 0, 2) { throw "Elevated PHONE_CAMERA uninstall failed (exit $($elevated.ExitCode))." }
    Remove-UserSessionState
    if ($elevated.ExitCode -eq 2) {
        Write-Warning 'PHONE_CAMERA machine files remain locked; user settings were removed, but the remaining folder needs manual cleanup after closing camera apps.'
        exit 2
    }
    Write-Host 'PHONE_CAMERA user settings removed in the original user session.' -ForegroundColor Green
    return
}

function Get-SafeInstallDirectory {
    $programFiles = [IO.Path]::GetFullPath($env:ProgramFiles)
    $expected = [IO.Path]::GetFullPath((Join-Path -Path $programFiles -ChildPath 'PHONE_CAMERA'))
    $literalExpected = Join-Path -Path $programFiles -ChildPath 'PHONE_CAMERA'
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals($expected, $literalExpected)) {
        throw "Refusing to remove an unexpected PHONE_CAMERA path: $expected"
    }
    if (-not (Test-Path -LiteralPath $expected)) { return $expected }

    $root = Get-Item -LiteralPath $expected -Force -ErrorAction Stop
    if (-not $root.PSIsContainer -or (($root.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0)) {
        throw "Refusing to remove PHONE_CAMERA because its install directory is not a normal directory: $expected"
    }
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($expected)
    while ($pending.Count -gt 0) {
        $current = $pending.Pop()
        foreach ($item in Get-ChildItem -LiteralPath $current -Force -ErrorAction Stop) {
            if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing to remove PHONE_CAMERA because it contains a reparse point: $($item.FullName)"
            }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) }
        }
    }
    return $expected
}

function Get-RemovalExitCode([bool]$Remaining) {
    if ($Remaining) { return 2 }
    return 0
}

$dest = Get-SafeInstallDirectory
$exe = 'PHONE_CAMERA.exe'
if (Test-Path "$dest\$exe") {
    & "$dest\$exe" --quit | Out-Null
}
foreach ($dll in 'MyCamVCam.dll', 'MyCamVCamDShow.dll') {
    if (Test-Path "$dest\$dll") {
        $r = Start-Process regsvr32.exe -ArgumentList "/s /u `"$dest\$dll`"" -Wait -PassThru
        if ($r.ExitCode -ne 0) { Write-Warning "Could not unregister $dll; close apps using PHONE_CAMERA and retry." }
    }
}
& schtasks.exe /Delete /TN 'PHONE_CAMERA phone driver' /F 2>$null | Out-Null
& netsh.exe advfirewall firewall delete rule name="PHONE_CAMERA iPhone camera" 2>$null | Out-Null
$deleteTarget = Get-SafeInstallDirectory
if (Test-Path -LiteralPath $deleteTarget) {
    Remove-Item -LiteralPath $deleteTarget -Recurse -Force -ErrorAction SilentlyContinue
}
$remaining = Test-Path -LiteralPath $dest

if ($RemoveUsbDk) {
    $pkg = Get-Package -Name 'UsbDk*' -ErrorAction SilentlyContinue
    if ($pkg) { $pkg | Uninstall-Package -Force | Out-Null }
}
if ($ElevatedChild) {
    if ($remaining) {
        Write-Warning 'PHONE_CAMERA machine files remain locked; close camera apps before removing the remaining folder.'
    } else {
        Write-Host 'PHONE_CAMERA machine uninstall completed; returning to the original user session for HKCU cleanup.'
    }
    exit (Get-RemovalExitCode $remaining)
}
Remove-UserSessionState
if ($remaining) {
    Write-Warning 'PHONE_CAMERA was unregistered, but some files remain because a camera app still has them open. Close that app and remove the remaining PHONE_CAMERA folder manually; Frame Server and existing MyCam were not touched.'
    exit (Get-RemovalExitCode $remaining)
} else {
    Write-Host 'PHONE_CAMERA removed. Existing MyCam installation, processes and Frame Server were not touched.' -ForegroundColor Green
}
