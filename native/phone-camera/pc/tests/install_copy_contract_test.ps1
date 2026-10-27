param(
    [Parameter(Mandatory = $false)]
    [string]$InstallScript
)

$ErrorActionPreference = 'Stop'
if ([string]::IsNullOrWhiteSpace($InstallScript)) { $InstallScript = Join-Path $PSScriptRoot '..\install.ps1' }
$tokens = $null
$errors = $null
$resolvedScript = (Resolve-Path -LiteralPath $InstallScript).Path
$installText = Get-Content -LiteralPath $resolvedScript -Raw
$ast = [System.Management.Automation.Language.Parser]::ParseFile($resolvedScript, [ref]$tokens, [ref]$errors)
if ($errors.Count -gt 0) { throw "PowerShell parse failed: $($errors[0].Message)" }

$copyCommands = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Copy-Item'
}, $true))
if ($copyCommands.Count -ne 4) { throw "Expected four artifact Copy-Item commands, found $($copyCommands.Count)." }
foreach ($command in $copyCommands) {
    $elements = @($command.CommandElements | ForEach-Object { $_.Extent.Text })
    if ($elements -notcontains '-LiteralPath' -or $elements -notcontains '-Destination') {
        throw "Copy-Item must use explicit -LiteralPath and -Destination: $($command.Extent.Text)"
    }
}

$newItemCommands = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and
    $node.GetCommandName() -eq 'New-Item' -and $node.Extent.Text -match '-ItemType\s+File'
}, $true))
if ($newItemCommands.Count -ne 1) { throw "Expected one unique MSI reservation command, found $($newItemCommands.Count)." }
$newItemElements = @($newItemCommands[0].CommandElements | ForEach-Object { $_.Extent.Text })
if ($newItemElements -notcontains '-Path' -or $newItemElements -contains '-LiteralPath') {
    throw "MSI reservation must bind New-Item through -Path only: $($newItemCommands[0].Extent.Text)"
}
$tempMsi = Join-Path ([IO.Path]::GetTempPath()) ("PHONE_CAMERA-new-item-{0}.tmp" -f [guid]::NewGuid())
try {
    $msi = $tempMsi
    & ([scriptblock]::Create($newItemCommands[0].Extent.Text)) | Out-Null
    if (-not (Test-Path -LiteralPath $tempMsi -PathType Leaf)) { throw 'New-Item did not create the reserved temporary file.' }
} finally {
    if (Test-Path -LiteralPath $tempMsi) { Remove-Item -LiteralPath $tempMsi -Force -ErrorAction SilentlyContinue }
}

$cleanupGuards = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.IfStatementAst] -and
    $node.Extent.Text -match '^\s*if\s*\(\$reserved\s+-and[\s\S]*Remove-Item\s+-LiteralPath\s+\$msi'
}, $true))
if ($cleanupGuards.Count -ne 1) { throw 'Expected one reserved-only MSI cleanup guard.' }
$foreignMsi = Join-Path ([IO.Path]::GetTempPath()) ("PHONE_CAMERA-foreign-{0}.msi" -f [guid]::NewGuid())
$cleanupCalled = $false
Set-Content -LiteralPath $foreignMsi -Value 'foreign-file' -Encoding UTF8
function Remove-Item { $script:cleanupCalled = $true }
try {
    $reserved = $false
    $msi = $foreignMsi
    & ([scriptblock]::Create($cleanupGuards[0].Extent.Text))
    if ($cleanupCalled) { throw 'MSI cleanup ran even though reservation failed.' }
    if (-not (Test-Path -LiteralPath $foreignMsi -PathType Leaf)) { throw 'Foreign temp file was removed.' }
} finally {
    Microsoft.PowerShell.Management\Remove-Item -LiteralPath Function:\Remove-Item -Force -ErrorAction SilentlyContinue
    Microsoft.PowerShell.Management\Remove-Item -LiteralPath $foreignMsi -Force -ErrorAction SilentlyContinue
}

$setCommands = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Set-ItemProperty'
}, $true))
$allStartCommands = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq 'Start-Process'
}, $true))
$startCommands = @($allStartCommands | Where-Object { $_.Extent.Text -match 'Start-Process\s+(?:-FilePath\s+)?explorer\.exe' })
if ($setCommands.Count -ne 1) { throw "Expected one Set-ItemProperty command, found $($setCommands.Count)." }
if ($startCommands.Count -ne 1) { throw "Expected one separate Start-Process command, found $($startCommands.Count)." }
if ($startCommands[0].Extent.Text -notmatch 'Start-Process\s+(?:-FilePath\s+)?explorer\.exe') {
    throw "The separate launch command must target explorer.exe: $($startCommands[0].Extent.Text)"
}
$uacCommands = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and
    $node.GetCommandName() -eq 'Start-Process' -and $node.Extent.Text -match 'Start-Process\s+powershell'
}, $true))
if ($uacCommands.Count -ne 1 -or $uacCommands[0].Extent.Text -notmatch '-Wait' -or
    $uacCommands[0].Extent.Text -notmatch '-PassThru' -or $installText -notmatch 'ElevatedChild') {
    throw 'UAC handoff must wait for the elevated child and pass -ElevatedChild.'
}
$setElements = @($setCommands[0].CommandElements)
$valueIndex = -1
for ($i = 0; $i -lt $setElements.Count; $i++) {
    if ($setElements[$i].Extent.Text -eq '-Value') { $valueIndex = $i; break }
}
if ($valueIndex -lt 0 -or $valueIndex + 1 -ge $setElements.Count) { throw 'Set-ItemProperty has no parseable -Value expression.' }

$configureFunction = @($ast.FindAll({ param($node) $node.GetType().Name -eq 'FunctionDefinitionAst' -and $node.Name -eq 'Configure-UserSession' }, $true))
if ($configureFunction.Count -ne 1 -or $installText -notmatch '\$StartWithWindows') { throw 'Configure-UserSession lacks explicit StartWithWindows opt-in.' }
$startupCalled = $false
$launchCount = 0
function Set-ItemProperty { param($Path, $Name, $Value) $script:startupCalled = $true }
function Start-Process { param($FilePath, $ArgumentList) $script:launchCount++ }
try {
    $functionText = $configureFunction[0].Extent.Text
    & ([scriptblock]::Create("$functionText; Configure-UserSession -InstallDirectory 'C:\PHONE_CAMERA-test' -Executable 'PHONE_CAMERA.exe'"))
    if ($startupCalled -or $launchCount -ne 1) { throw 'Default Configure-UserSession changed autostart or did not launch exactly once.' }
    $startupCalled = $false
    & ([scriptblock]::Create("$functionText; Configure-UserSession -InstallDirectory 'C:\PHONE_CAMERA-test' -Executable 'PHONE_CAMERA.exe' -StartWithWindows"))
    if (-not $startupCalled -or $launchCount -ne 2) { throw 'StartWithWindows opt-in did not configure autostart and launch.' }
} finally {
    Microsoft.PowerShell.Management\Remove-Item -LiteralPath Function:\Set-ItemProperty -Force -ErrorAction SilentlyContinue
    Microsoft.PowerShell.Management\Remove-Item -LiteralPath Function:\Start-Process -Force -ErrorAction SilentlyContinue
}

$tempRoot = Join-Path ([IO.Path]::GetTempPath()) ("phone-camera-copy-contract-{0}" -f [guid]::NewGuid())
$buildDir = Join-Path $tempRoot 'build'
$destination = Join-Path $tempRoot 'destination'
$created = $false
try {
    New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
    New-Item -ItemType Directory -Path $destination -Force | Out-Null
    $created = $true
    $artifacts = @('PHONE_CAMERA.exe', 'PHONE_CAMERA_libusb.dll', 'MyCamVCam.dll', 'MyCamVCamDShow.dll')
    foreach ($artifact in $artifacts) {
        Set-Content -LiteralPath (Join-Path $buildDir $artifact) -Value "copy-contract:$artifact" -Encoding UTF8
    }

    # Execute only the three extracted copy commands. The install script itself is never dot-sourced.
    $BuildDir = $buildDir
    $dest = $destination
    $exe = 'PHONE_CAMERA.exe'
    foreach ($command in $copyCommands) {
        & ([scriptblock]::Create($command.Extent.Text))
    }
    foreach ($artifact in $artifacts) {
        $target = Join-Path $destination $artifact
        if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw "Missing copied artifact: $artifact" }
        $expected = "copy-contract:$artifact"
        if ((Get-Content -LiteralPath $target -Raw).Trim() -ne $expected) { throw "Copied content mismatch: $artifact" }
    }
    $executablePath = Join-Path -Path $destination -ChildPath $exe
    $runValue = & ([scriptblock]::Create($setElements[$valueIndex + 1].Extent.Text))
    $expectedRunValue = '"' + (Join-Path $destination $exe) + '"'
    if ($runValue -ne $expectedRunValue) { throw "Unexpected autostart value: $runValue" }
    Write-Output 'PHONE_CAMERA install copy/launch contract passed in temporary directories.'
} finally {
    if ($created -and (Test-Path -LiteralPath $tempRoot)) {
        Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
