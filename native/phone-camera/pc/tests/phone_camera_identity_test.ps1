param(
    [Parameter(Mandatory = $false)]
    [string]$Root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
)

$ErrorActionPreference = 'Stop'
$failures = [System.Collections.Generic.List[string]]::new()

function Read-Source([string]$RelativePath) {
    $path = Join-Path $Root $RelativePath
    if (-not (Test-Path -LiteralPath $path)) {
        $failures.Add("missing $RelativePath")
        return ''
    }
    return Get-Content -LiteralPath $path -Raw
}

function Require-Text([string]$RelativePath, [string]$Pattern, [string]$Description) {
    $text = Read-Source $RelativePath
    if ($text -notmatch $Pattern) { $failures.Add("${RelativePath}: $Description") }
}

function Forbid-Text([string]$RelativePath, [string]$Pattern, [string]$Description) {
    $text = Read-Source $RelativePath
    if ($text -match $Pattern) { $failures.Add("${RelativePath}: $Description") }
}

function Require-File([string]$RelativePath, [string]$Description) {
    if (-not (Test-Path -LiteralPath (Join-Path $Root $RelativePath) -PathType Leaf)) {
        $failures.Add("${RelativePath}: $Description")
    }
}

# Product-owned identities must not overlap with the installed MyCam package.
Require-Text 'common/guids.h' 'A43186E6-3C6E-4FC2-8E62-7193B5AA0FF3' 'new MF CLSID string is missing'
Require-Text 'common/guids.h' 'F1CAA121-F8A7-4DFB-BEAB-1DED6A7887EA' 'new DirectShow CLSID string is missing'
Require-Text 'vcam/dllmain.cpp' 'DECLSPEC_UUID\("A43186E6-3C6E-4FC2-8E62-7193B5AA0FF3"\)' 'MF class UUID does not match the PHONE_CAMERA CLSID'
Require-Text 'vcam-dshow/filter.h' 'DECLSPEC_UUID\("F1CAA121-F8A7-4DFB-BEAB-1DED6A7887EA"\)' 'DirectShow class UUID does not match the PHONE_CAMERA CLSID'
Require-Text 'CMakeLists.txt' 'OUTPUT_NAME\s+PHONE_CAMERA' 'the companion output is not PHONE_CAMERA.exe'
Require-Text 'CMakeLists.txt' 'add_library\(usb\s+SHARED' 'Windows libusb target is not shared'
Require-Text 'CMakeLists.txt' 'WINDOWS_EXPORT_ALL_SYMBOLS\s+ON' 'Windows libusb exports are not enabled'
Require-Text 'common/shared_frame.h' 'Global\\\\PHONE_CAMERAFrame' 'global frame mapping is not isolated'
Require-Text 'common/shared_frame.h' 'PHONE_CAMERAFrame' 'local frame mapping is not isolated'
Require-Text 'common/guids.h' 'kSettingsKey\[\].*Software\\\\PHONE_CAMERA' 'HKCU settings are not isolated'
Require-Text 'common/guids.h' 'kRunValue\[\].*PHONE_CAMERA' 'Run value is not isolated'
Require-Text 'common/guids.h' 'kCompanionMutexName\[\].*Local\\\\PHONE_CAMERACompanion' 'mutex is not isolated'
Require-Text 'common/guids.h' 'kCompanionWindowClass\[\].*PHONE_CAMERACompanion' 'window class is not isolated'
Require-Text 'companion/log.cpp' 'kCompanionLogFile' 'companion log is not isolated'
Require-Text 'common/guids.h' 'kCertificateFriendlyName\[\].*PHONE_CAMERA web camera' 'certificate friendly identity is not isolated'
Require-Text 'common/guids.h' 'kDriverTaskName\[\].*PHONE_CAMERA phone driver' 'scheduled task name is not isolated'
Require-Text 'common/guids.h' 'kFirewallRuleName\[\].*PHONE_CAMERA iPhone camera' 'firewall rule name is not isolated'

# Installer identity and directory must be unique, and install/uninstall must not kill unrelated processes
# or the shared Windows Frame Server service.
Require-Text '../installer/mycam.iss' '5FB4B81E-8CDC-44C8-A79B-6370B21FED09' 'installer AppId is missing'
Require-Text '../installer/mycam.iss' 'AppName=PHONE_CAMERA' 'installer display name is missing'
Require-Text '../installer/mycam.iss' 'DefaultDirName=\{autopf\}\\PHONE_CAMERA' 'installer destination is not isolated'
Require-Text '../installer/mycam.iss' '\{#BuildDir\}\\PHONE_CAMERA\.exe' 'installer does not package PHONE_CAMERA.exe'
Require-Text '../installer/mycam.iss' '\{#BuildDir\}\\PHONE_CAMERA_libusb\.dll' 'installer does not package the relinkable libusb DLL'
Require-Text '../installer/mycam.iss' 'Source: "\{#BuildDir\}\\PHONE_CAMERA_libusb\.dll"; DestDir: "\{tmp\}"; Flags: dontcopy' 'upgrade helper does not extract PHONE_CAMERA_libusb.dll'
$issText = Read-Source '../installer/mycam.iss'
$extractLibusbAt = $issText.IndexOf("ExtractTemporaryFile('PHONE_CAMERA_libusb.dll')")
$extractExeAt = $issText.IndexOf("ExtractTemporaryFile('PHONE_CAMERA.exe')")
$quitAt = $issText.IndexOf("RunHidden(ExpandConstant('{tmp}\PHONE_CAMERA.exe'), '--quit')")
if ($extractLibusbAt -lt 0 -or $extractExeAt -lt 0 -or $quitAt -lt 0 -or $extractLibusbAt -gt $extractExeAt -or $extractExeAt -gt $quitAt) {
    $failures.Add('../installer/mycam.iss: temp libusb extraction must precede exe extraction and --quit')
}
Require-Text '../installer/mycam.iss' '\{#BuildDir\}\\MyCamVCam\.dll' 'installer does not package the MF backend'
Require-Text '../installer/mycam.iss' '\{#BuildDir\}\\MyCamVCamDShow\.dll' 'installer does not package the DirectShow backend'
Require-Text '../installer/mycam.iss' 'PHONE_CAMERA-MIT-LICENSE\.txt' 'installer does not include the MIT notice'
Require-Text '../installer/mycam.iss' 'MyCam-MIT-LICENSE\.txt' 'installer does not preserve the upstream MyCam notice'
Require-Text '../installer/mycam.iss' 'qrcodegen-MIT-LICENSE\.txt' 'installer does not include qrcodegen notice'
Require-Text '../installer/mycam.iss' 'DejaVu-LICENSE\.txt' 'installer does not include DejaVu notice'
Require-File '../LICENSE' 'upstream MyCam license source is missing'
Require-File '../../..\LICENSE' 'PHONE_CAMERA license source is missing'
Require-File 'third_party/qrcodegen/LICENSE' 'qrcodegen license source is missing'
Require-File '../app/src/main/assets/licenses/DejaVu-LICENSE.txt' 'DejaVu license source is missing'
Require-Text '../installer/mycam.iss' 'CloseApplications=no' 'installer may close other applications'
Require-Text '../installer/mycam.iss' 'RestartApplications=no' 'installer may restart other applications'
Require-Text '../installer/mycam.iss' 'original user session' 'installer does not explain original-user startup configuration'
Forbid-Text '../installer/mycam.iss' 'ValueName: ""PHONE_CAMERA""|Software\\Microsoft\\Windows\\CurrentVersion\\Run' 'installer writes HKCU autostart from elevated context'
Forbid-Text '../installer/mycam.iss' 'WizardImageFile|WizardSmallImageFile|art\\\\' 'installer still embeds inherited MyCam artwork'
Require-Text '../installer/mycam.iss' 'Parameters: "--register-task"' 'installer has no USB driver task'
Require-Text '../installer/mycam.iss' 'Tasks: usbdriver' 'USB driver task is not limited to the USB scenario'
Require-Text 'install.ps1' 'PHONE_CAMERA\.exe' 'script does not install PHONE_CAMERA.exe'
Require-Text 'install.ps1' 'PHONE_CAMERA_libusb\.dll' 'script does not install the relinkable libusb DLL'
Require-Text 'install.ps1' 'StartWithWindows' 'source setup lacks explicit autostart opt-in'
Require-Text 'install.ps1' 'PHONE_CAMERA-UsbDk-' 'UsbDk temp file is not per-run unique'
Require-Text 'install.ps1' 'New-Item -ItemType File -Path \$msi' 'UsbDk temp reservation does not use New-Item -Path'
Require-Text 'install.ps1' '\$reserved\s+-and\s+\$msi' 'UsbDk cleanup is not guarded by successful reservation'
Forbid-Text 'install.ps1' 'New-Item[^\r\n]*-LiteralPath \$msi' 'UsbDk temp reservation uses unsupported New-Item -LiteralPath'
Forbid-Text 'install.ps1' 'TEMP\\UsbDk_1\.0\.22_x64\.msi' 'script may overwrite a fixed temp MSI'
Require-Text 'uninstall.ps1' 'PHONE_CAMERA' 'script does not target PHONE_CAMERA identities'
Require-Text 'uninstall.ps1' 'Get-SafeInstallDirectory' 'uninstaller lacks an install-path safety guard'
Require-Text 'uninstall.ps1' 'ReparsePoint' 'uninstaller does not reject reparse points'
Require-Text 'uninstall.ps1' 'Remove-Item -LiteralPath \$deleteTarget' 'uninstaller delete is not bound to the validated literal path'
Require-Text 'uninstall.ps1' 'Remove-UserSessionState' 'uninstaller lacks original-user HKCU cleanup'
Require-Text 'uninstall.ps1' 'ElevatedChild' 'uninstaller lacks the UAC handoff marker'
Require-Text 'uninstall.ps1' 'Start-Process powershell.*-Wait -PassThru' 'uninstaller does not wait for elevated machine cleanup'
$uninstallText = Read-Source 'uninstall.ps1'
$remainingAt = $uninstallText.IndexOf('$remaining = Test-Path')
$childAt = $uninstallText.IndexOf('if ($ElevatedChild)')
if ($remainingAt -lt 0 -or $childAt -lt $remainingAt) { $failures.Add('uninstall.ps1: locked-file result is checked after machine cleanup and before elevated-child return') }
$uTokens = $null
$uErrors = $null
$uAst = [System.Management.Automation.Language.Parser]::ParseInput($uninstallText, [ref]$uTokens, [ref]$uErrors)
$exitFunction = @($uAst.FindAll({ param($node) $node.GetType().Name -eq 'FunctionDefinitionAst' -and $node.Name -eq 'Get-RemovalExitCode' }, $true))
if ($exitFunction.Count -ne 1) { $failures.Add('uninstall.ps1: missing removal exit-code helper') }
else {
    $fn = $exitFunction[0].Extent.Text
    $noRemaining = & ([scriptblock]::Create("$fn; Get-RemovalExitCode `$false"))
    $remaining = & ([scriptblock]::Create("$fn; Get-RemovalExitCode `$true"))
    if ($noRemaining -ne 0 -or $remaining -ne 2) { $failures.Add('uninstall.ps1: removal exit-code helper does not distinguish locked files') }
}
Forbid-Text 'install.ps1' 'Stop-Process|taskkill|Restart-Service\s+FrameServer|net\.exe\s+stop\s+FrameServer' 'installer script forcibly stops processes or Frame Server'
Forbid-Text 'uninstall.ps1' 'Stop-Process|taskkill|Restart-Service\s+FrameServer|net\.exe\s+stop\s+FrameServer' 'uninstaller script forcibly stops processes or Frame Server'
Forbid-Text '../installer/mycam.iss' 'taskkill\.exe|net\.exe.*stop\s+FrameServer|Stop-Process|Restart-Service\s+FrameServer' 'installer forcibly stops processes or Frame Server'

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ -ErrorAction Continue }
    exit 1
}

Write-Output 'PHONE_CAMERA identity and installer isolation checks passed.'
