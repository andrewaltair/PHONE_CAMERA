param(
  [string]$BuildDir = '',
  [string]$Generator = 'Visual Studio 17 2022'
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'native\phone-camera\pc'
if (-not $BuildDir) { $BuildDir = Join-Path $source 'build' }

function Find-CMake {
  $command = Get-Command cmake.exe -ErrorAction SilentlyContinue
  if ($command) { return $command.Source }
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path -LiteralPath $vswhere) {
    $installs = & $vswhere -all -products '*' -property installationPath
    foreach ($install in $installs) {
      $candidate = Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
      if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
  }
  throw 'CMake was not found. Install CMake or the Visual Studio C++ CMake tools, then retry.'
}

if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT) {
  throw 'The Windows build requires Windows; see README.md for the Linux preview.'
}
$cmake = Find-CMake
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $ctest)) { throw 'CTest was not found beside CMake.' }

& $cmake -S $source -B $BuildDir -G $Generator -A x64 -DMYCAM_BUILD_TOOLS=ON
if ($LASTEXITCODE -ne 0) { throw 'Native CMake configuration failed.' }
& $cmake --build $BuildDir --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Native Windows build failed.' }
& $ctest --test-dir $BuildDir -C Release --output-on-failure
if ($LASTEXITCODE -ne 0) { throw 'Native Windows tests failed.' }

Write-Host "Built and tested PHONE_CAMERA in $BuildDir\Release"
