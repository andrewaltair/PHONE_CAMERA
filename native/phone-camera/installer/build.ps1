<#
  Builds the PHONE_CAMERA installer: installer\output\PHONE_CAMERA-Setup-<version>.exe
    powershell -ExecutionPolicy Bypass -File installer\build.ps1 [-Version 1.0.0]

  1. Builds the PC side (Release, pc\build-release) and runs the unit tests.
  2. Downloads the UsbDk driver package (hash-checked) and its license into installer\redist.
  3. Compiles installer\mycam.iss with Inno Setup 6 (winget install JRSoftware.InnoSetup).
#>
param([string]$Version = '1.5.1')
$ErrorActionPreference = 'Stop'
$root = Resolve-Path "$PSScriptRoot\.."

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = 'C:\Program Files\CMake\bin\cmake.exe' }
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'
$iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe") |
    Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 not found. Install it: winget install JRSoftware.InnoSetup' }

# --- 1. Build and test ---------------------------------------------------------------------------
$build = Join-Path $root 'pc\build-release'
& $cmake -S (Join-Path $root 'pc') -B $build -G 'Visual Studio 17 2022' -A x64 | Out-Null
& $cmake --build $build --config Release
if ($LASTEXITCODE) { throw 'PC build failed' }
& $ctest --test-dir $build -C Release --output-on-failure
if ($LASTEXITCODE) { throw 'Unit tests failed' }

# --- 2. Third-party driver package ----------------------------------------------------------------
$redist = Join-Path $PSScriptRoot 'redist'
New-Item -ItemType Directory -Force $redist | Out-Null
$msi = Join-Path $redist 'UsbDk_1.0.22_x64.msi'
$sha256 = '91F6F695E1E13C656024E6D3B55620BF08D8835EF05EE0496935BA6BB62466A5'
if (-not (Test-Path $msi) -or (Get-FileHash $msi -Algorithm SHA256).Hash -ne $sha256) {
    Invoke-WebRequest 'https://github.com/daynix/UsbDk/releases/download/v1.00-22/UsbDk_1.0.22_x64.msi' -OutFile $msi -UseBasicParsing
    if ((Get-FileHash $msi -Algorithm SHA256).Hash -ne $sha256) { Remove-Item $msi; throw 'UsbDk download hash mismatch' }
}
$license = Join-Path $redist 'UsbDk-LICENSE.txt'
if (-not (Test-Path $license)) {
    Invoke-WebRequest 'https://raw.githubusercontent.com/daynix/UsbDk/v1.00-22/LICENSE' -OutFile $license -UseBasicParsing
}

# --- 3. Installer ---------------------------------------------------------------------------------
& $iscc "/DAppVersion=$Version" "/DBuildDir=$build\Release" (Join-Path $PSScriptRoot 'mycam.iss')
if ($LASTEXITCODE) { throw 'Inno Setup failed' }
Write-Host "`nBuilt installer\output\PHONE_CAMERA-Setup-$Version.exe" -ForegroundColor Green
