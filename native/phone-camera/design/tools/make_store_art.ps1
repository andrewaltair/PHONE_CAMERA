<#
  Renders the Google Play store listing art in the Windows XP style: the "Snap" camera over the sunny hills.
    powershell -ExecutionPolicy Bypass -File design\tools\make_store_art.ps1

  Outputs
    design\assets\store\play-icon-512.png        512 x 512, 32-bit PNG, full-bleed square (Play rounds the corners)
    design\assets\store\feature-graphic.png      1024 x 500, 24-bit PNG (no alpha, as Play requires)
    design\previews\store-art.png                review sheet
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\luna_draw.ps1"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$storeDir = Join-Path $root 'design\assets\store'
New-Item -ItemType Directory -Force $storeDir | Out-Null

# --- 512 px icon: the launcher tile (hills + camera), edge to edge ----------------------------------
function Store-Icon([int]$s) {
    $bmp = Paint-Hills $s $s -horizon 0.62
    $g = [System.Drawing.Graphics]::FromImage($bmp); Set-Quality $g
    # Play masks to a rounded square (about 20 % radius); keep the camera well inside it.
    $ic = [int]($s * 0.74)
    $cam = Render-Icon $ic 'ready'
    $g.DrawImage($cam, [int](($s - $ic) / 2), [int](($s - $ic) / 2 + $s * 0.01), $ic, $ic); $cam.Dispose()
    $g.Dispose()
    $bmp
}

# --- 1024 x 500 feature graphic --------------------------------------------------------------------
function Feature-Graphic([int]$w, [int]$h) {
    # The hills photograph, cropped to the banner shape.
    $bmp = Photo-Scene $w $h -anchorY 0.40
    $g = [System.Drawing.Graphics]::FromImage($bmp); Set-Quality $g

    # Camera on the left third.
    $s = [int]($h * 0.72); $x = [int]($w * 0.07); $y = [int](($h - $s) / 2 - $h * 0.03)
    $cam = Render-Icon $s 'ready'
    $g.DrawImage($cam, $x, $y, $s, $s); $cam.Dispose()
    $g.Dispose()

    # Title block on the right, over the sky. Play may overlay a play button in the centre for videos
    # only, so the text sits right of centre and away from the edges.
    $tx = $w * 0.46
    $title = New-Object System.Drawing.Font('Trebuchet MS', [single]($h * 0.22), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $tag = New-Object System.Drawing.Font('Tahoma', [single]($h * 0.07), [System.Drawing.GraphicsUnit]::Pixel)
    $small = New-Object System.Drawing.Font('Tahoma', [single]($h * 0.044), [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
    $ty = $h * 0.16
    Draw-SoftText $bmp 'MyCam' $title (Hex '#FFFFFF') $tx $ty 4 200
    Draw-SoftText $bmp 'Your phone, your webcam' $tag (Hex '#FFFFFF') ($tx + $h * 0.02) ($ty + $h * 0.27) 2.5 210
    $dot = [string][char]0x00B7
    $line = "Wired USB  $dot  Plug and play  $dot  Windows 11"
    Draw-SoftText $bmp $line $small (Hex '#FFFFFF') ($tx + $h * 0.022) ($ty + $h * 0.39) 2 200
    $bmp
}

$icon = Store-Icon 512
$icon.Save((Join-Path $storeDir 'play-icon-512.png'), [System.Drawing.Imaging.ImageFormat]::Png)
$feature = To-Rgb24 (Feature-Graphic 1024 500)
$feature.Save((Join-Path $storeDir 'feature-graphic.png'), [System.Drawing.Imaging.ImageFormat]::Png)

# Review sheet: the feature graphic, and the icon with Play's rounded mask.
$sheet = New-Object System.Drawing.Bitmap 1592, 540
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'
$g.Clear((Hex '#ECE9D8'))
$g.DrawImage($feature, 20, 20)
$ox = 1060; $oy = 20
$mask = RoundRect-Path $ox $oy 512 512 (512 * 0.2)
$tb = New-Object System.Drawing.TextureBrush($icon)
$tb.TranslateTransform($ox, $oy)
$g.FillPath($tb, $mask); $tb.Dispose()
$g.Dispose()
$sheet.Save((Join-Path $root 'design\previews\store-art.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote design\assets\store\*.png and design\previews\store-art.png'
