<#
  Renders the installer wizard artwork in the Windows XP style: a side panel with the "Snap" camera over
  the sunny hills, and a small header image.
    powershell -ExecutionPolicy Bypass -File design\tools\make_installer_art.ps1

  Outputs installer\art\wizard-<scale>.bmp and installer\art\small-<scale>.bmp at the sizes Inno Setup's
  modern wizard expects for 100/150/200 % display scaling, plus design\previews\installer-art.png.
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\luna_draw.ps1"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$artDir = Join-Path $root 'installer\art'
New-Item -ItemType Directory -Force $artDir | Out-Null

function Wizard-Image([int]$w, [int]$h) {
    # Tall panel: lots of sky, the grass along the bottom third. Cropped from the hills photograph, zoomed
    # in a little so the panel gets a gentle slope rather than a narrow slice of it.
    $bmp = Photo-Scene $w $h -zoom 1.3 -anchorY 0.05
    $g = [System.Drawing.Graphics]::FromImage($bmp); Set-Quality $g
    $s = [int]($w * 0.7); $x = [int](($w - $s) / 2); $y = [int]($h * 0.13)
    $cam = Render-Icon $s 'ready'
    $g.DrawImage($cam, $x, $y, $s, $s); $cam.Dispose()
    $g.Dispose()

    $title = New-Object System.Drawing.Font('Trebuchet MS', [single]($w * 0.19), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $tag = New-Object System.Drawing.Font('Tahoma', [single]($w * 0.072), [System.Drawing.GraphicsUnit]::Pixel)
    $mg = [System.Drawing.Graphics]::FromImage($bmp)
    $tw = $mg.MeasureString('MyCam', $title).Width; $gw = $mg.MeasureString('Your phone, your webcam', $tag).Width
    $mg.Dispose()
    $ty = $y + $s + $h * 0.0
    Draw-SoftText $bmp 'MyCam' $title (Hex '#FFFFFF') (($w - $tw) / 2) $ty ([Math]::Max(1.5, $w / 90)) 190
    Draw-SoftText $bmp 'Your phone, your webcam' $tag (Hex '#FFFFFF') (($w - $gw) / 2) ($ty + $w * 0.23) ([Math]::Max(1.2, $w / 110)) 200
    $bmp
}

function Small-Image([int]$w, [int]$h) {
    $bmp, $g = New-Canvas $w $h $false
    $g.Clear((Hex '#FFFFFF')) # Inno's header strip is white.
    $s = [int]([Math]::Min($w, $h) * 0.98)
    $cam = Render-Icon $s 'ready'
    $g.DrawImage($cam, [int](($w - $s) / 2), [int](($h - $s) / 2), $s, $s); $cam.Dispose()
    $g.Dispose()
    $bmp
}

# Inno Setup 6 modern-wizard sizes for 100 %, 150 % and 200 % scaling.
$wizard = @{ 100 = @(164, 314); 150 = @(246, 459); 200 = @(328, 604) }
$smallSizes = @{ 100 = @(55, 55); 150 = @(83, 80); 200 = @(110, 106) }
$previews = @()
foreach ($scale in 100, 150, 200) {
    $b = To-Rgb24 (Wizard-Image $wizard[$scale][0] $wizard[$scale][1])
    $b.Save((Join-Path $artDir "wizard-$scale.bmp"), [System.Drawing.Imaging.ImageFormat]::Bmp)
    $sb = To-Rgb24 (Small-Image $smallSizes[$scale][0] $smallSizes[$scale][1])
    $sb.Save((Join-Path $artDir "small-$scale.bmp"), [System.Drawing.Imaging.ImageFormat]::Bmp)
    $previews += , @($b, $sb)
}
$sheet = New-Object System.Drawing.Bitmap 920, 620
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.Clear((Hex '#ECE9D8'))
$g.DrawImage($previews[2][0], 8, 8)
$g.DrawImage($previews[1][0], 346, 8)
$g.DrawImage($previews[0][0], 602, 8)
$g.DrawImage($previews[2][1], 780, 8)
$g.DrawImage($previews[1][1], 780, 130)
$g.DrawImage($previews[0][1], 780, 226)
$g.Dispose()
$sheet.Save((Join-Path $root 'design\previews\installer-art.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote installer\art\*.bmp and design\previews\installer-art.png'
