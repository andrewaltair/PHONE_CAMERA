param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,
    [Parameter(Mandatory = $true)]
    [string]$Root
)

$ErrorActionPreference = 'Stop'
$release = (Resolve-Path -LiteralPath $BuildDir).Path
$dumpbin = (Get-Command dumpbin.exe -ErrorAction SilentlyContinue).Source
if (-not $dumpbin) {
    $hostLayouts = @('bin\Hostx64\x64\dumpbin.exe', 'bin\Hostx86\x64\dumpbin.exe')
    $toolRoots = @()
    if ($env:VCToolsInstallDir) { $toolRoots += $env:VCToolsInstallDir }
    if ($env:VCINSTALLDIR) { $toolRoots += (Join-Path $env:VCINSTALLDIR 'Tools\MSVC') }
    foreach ($toolRoot in $toolRoots) {
        foreach ($layout in $hostLayouts) {
            $candidate = Join-Path $toolRoot $layout
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $dumpbin = $candidate; break }
        }
        if ($dumpbin) { break }
    }
}
if (-not $dumpbin) {
    $vswhereCandidates = @(
        (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'),
        (Join-Path $env:ProgramFiles 'Microsoft Visual Studio\Installer\vswhere.exe')
    )
    $vswhere = $vswhereCandidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    if ($vswhere) {
        foreach ($pattern in @('VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe', 'VC\Tools\MSVC\*\bin\Hostx86\x64\dumpbin.exe')) {
            $found = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find $pattern 2>$null |
                Where-Object { $_ } | Select-Object -First 1
            if ($found) { $dumpbin = ([string]$found).Trim(); break }
        }
    }
}
if (-not $dumpbin) { throw 'dumpbin.exe is required for the PE relink check.' }

function Dump([string[]]$Arguments) {
    (& $dumpbin @Arguments 2>&1 | Out-String)
}

$companion = Join-Path $release 'PHONE_CAMERA.exe'
$libusb = Join-Path $release 'PHONE_CAMERA_libusb.dll'
$probe = Join-Path $release 'usb_probe.exe'
foreach ($path in @($companion, $libusb, $probe)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing tools-on artifact: $path" }
}

$deps = Dump @('/DEPENDENTS', $companion)
if ($deps -notmatch 'PHONE_CAMERA_libusb\.dll') { throw 'PHONE_CAMERA.exe does not import PHONE_CAMERA_libusb.dll.' }
$probeDeps = Dump @('/DEPENDENTS', $probe)
if ($probeDeps -notmatch 'PHONE_CAMERA_libusb\.dll') { throw 'usb_probe.exe does not import PHONE_CAMERA_libusb.dll.' }
$exports = Dump @('/EXPORTS', $libusb)
foreach ($api in @('libusb_init', 'libusb_set_option', 'libusb_open', 'libusb_close')) {
    if ($exports -notmatch "\b$api\b") { throw "PHONE_CAMERA_libusb.dll does not export $api." }
}

$install = Get-Content -LiteralPath (Join-Path $Root 'install.ps1') -Raw
$iss = Get-Content -LiteralPath (Join-Path $Root '..\installer\mycam.iss') -Raw
if ($install -notmatch 'PHONE_CAMERA_libusb\.dll') { throw 'install.ps1 does not copy PHONE_CAMERA_libusb.dll.' }
if ($iss -notmatch '\{#BuildDir\}\\PHONE_CAMERA_libusb\.dll') { throw 'installer does not package PHONE_CAMERA_libusb.dll.' }
Write-Output 'Windows libusb PE import/export and packaging contract passed.'
