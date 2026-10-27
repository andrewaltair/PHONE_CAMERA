<#
  Renders MyCam's Windows XP (Luna) style icons: the "Snap" camera logo and its status family.
  Re-run after changing the design:
    powershell -ExecutionPolicy Bypass -File design\tools\make_icons.ps1

  Outputs
    pc\companion\res\app.ico, tray_<state>.ico               Windows app + tray icons (16-256 px)
    design\assets\app-256.png                                large app icon for other artwork
    app\src\main\res\mipmap-*\ic_launcher_foreground.png     Android adaptive icon foreground (the camera)
    app\src\main\res\mipmap-*\ic_launcher(_round).png        Android legacy launcher icons (API < 26)
    app\src\main\res\drawable\ic_launcher_background.xml     adaptive background: the sunny hills (vector)
    app\src\main\res\drawable\ic_launcher_monochrome.xml     themed icon: body, bezel ring, hill line
    app\src\main\res\drawable\ic_stat_webcam.xml             notification icon: white camera outline
    app\src\main\res\mipmap-anydpi-v26\ic_launcher(_round).xml
    app\src\main\res\drawable-nodpi\status_<state>.png       Android status-card icons
    design\previews\icons.png                                review sheet
#>
$ErrorActionPreference = 'Stop'
. "$PSScriptRoot\luna_draw.ps1"
$root = (Resolve-Path "$PSScriptRoot\..\..").Path
$resDir = Join-Path $root 'pc\companion\res'
$previewDir = Join-Path $root 'design\previews'
$assetDir = Join-Path $root 'design\assets'
$androidRes = Join-Path $root 'app\src\main\res'
New-Item -ItemType Directory -Force $resDir, $previewDir, $assetDir | Out-Null

function Write-Ico([string]$path, [System.Drawing.Bitmap[]]$bitmaps) {
    $pngs = foreach ($b in $bitmaps) { $ms = New-Object System.IO.MemoryStream; $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); , $ms.ToArray() }
    $out = New-Object System.IO.MemoryStream
    $w = New-Object System.IO.BinaryWriter $out
    $w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$bitmaps.Count)
    $offset = 6 + 16 * $bitmaps.Count
    for ($i = 0; $i -lt $bitmaps.Count; $i++) {
        $d = if ($bitmaps[$i].Width -ge 256) { 0 } else { $bitmaps[$i].Width }
        $w.Write([byte]$d); $w.Write([byte]$d); $w.Write([byte]0); $w.Write([byte]0)
        $w.Write([UInt16]1); $w.Write([UInt16]32); $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
        $offset += $pngs[$i].Length
    }
    foreach ($p in $pngs) { $w.Write($p) }
    $w.Flush()
    [System.IO.File]::WriteAllBytes($path, $out.ToArray())
}

function Save-Png($bmp, [string]$path) {
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
}

function Write-Text([string]$path, [string]$text) {
    [System.IO.File]::WriteAllText($path, $text.Replace("`r`n", "`n"), (New-Object System.Text.UTF8Encoding $false))
}

# --- Windows icons --------------------------------------------------------------------------------
$sizes = 16, 20, 24, 32, 40, 48, 64, 256
$states = 'app', 'disconnected', 'ready', 'streaming', 'paused', 'error'
$rendered = @{}
foreach ($st in $states) {
    $drawState = if ($st -eq 'app') { 'ready' } else { $st }
    $rendered[$st] = foreach ($s in $sizes) { Render-Icon $s $drawState }
    $name = if ($st -eq 'app') { 'app.ico' } else { "tray_$st.ico" }
    Write-Ico (Join-Path $resDir $name) $rendered[$st]
    Write-Host "wrote $name"
}
Save-Png ($rendered['app'] | Where-Object { $_.Width -eq 256 }) (Join-Path $assetDir 'app-256.png')

# --- Android launcher -----------------------------------------------------------------------------
# The legacy tile: the hills scene with the camera on top.
function Launcher-Tile([int]$ls) {
    $tile = Paint-Hills $ls $ls -horizon 0.62
    $g = [System.Drawing.Graphics]::FromImage($tile); Set-Quality $g
    $ic = [int]($ls * 0.74)
    $cam = Render-Icon $ic 'ready'
    $g.DrawImage($cam, [int](($ls - $ic) / 2), [int](($ls - $ic) / 2 + $ls * 0.01), $ic, $ic); $cam.Dispose()
    $g.Dispose()
    $tile
}

# Adaptive foreground on a 108 dp canvas. The mark is rendered, measured, and then scaled and centred so
# that every one of its pixels lies inside the 66 dp safe circle: whatever mask a launcher applies -
# circle, squircle, rounded square - it can never clip the lens or the shutter. Returns the bitmap and
# the radius the mark ended up with, in dp, for the audit line this script prints.
function Adaptive-Foreground([double]$k) {
    $fs = [int](108 * $k)
    $s = [int](72 * $k)
    $cam = Render-Icon $s 'ready'
    $b = [LunaFx]::AlphaBounds($cam, 24)
    $bcx = $b[0] + $b[2] / 2.0; $bcy = $b[1] + $b[3] / 2.0
    $rad = [Math]::Sqrt([Math]::Pow($b[2] / 2.0, 2) + [Math]::Pow($b[3] / 2.0, 2))
    $f = [Math]::Min(1.0, (33.0 * $k) / $rad)
    $fg, $g = New-Canvas $fs $fs
    $g.DrawImage($cam, (RectF ($fs / 2 - $bcx * $f) ($fs / 2 - $bcy * $f) ($s * $f) ($s * $f)))
    $g.Dispose(); $cam.Dispose()
    @($fg, [Math]::Round($rad * $f / $k, 1))
}

$densities = [ordered]@{ mdpi = 1; hdpi = 1.5; xhdpi = 2; xxhdpi = 3; xxxhdpi = 4 }
foreach ($d in $densities.Keys) {
    $k = $densities[$d]

    $fg, $markRadius = Adaptive-Foreground $k
    if ($d -eq 'xxxhdpi') { Write-Host "adaptive foreground: the mark fits a $markRadius dp radius (safe zone is 33 dp)" }
    Save-Png $fg (Join-Path $androidRes "mipmap-$d\ic_launcher_foreground.png")

    # Legacy icons (API < 26): rounded square and round.
    $ls = [int](48 * $k)
    $tile = Launcher-Tile $ls
    foreach ($round in $false, $true) {
        $bmp, $g = New-Canvas $ls $ls
        $clip = if ($round) { Circle-Path ($ls / 2) ($ls / 2) ($ls / 2 - 1) } else { RoundRect-Path 1 1 ($ls - 2) ($ls - 2) ($ls * 0.18) }
        $tb = New-Object System.Drawing.TextureBrush($tile)
        $g.FillPath($tb, $clip); $tb.Dispose()
        $pen = New-Object System.Drawing.Pen((C 90 10 50 120), [single]([Math]::Max(1, $ls / 96)))
        $g.DrawPath($pen, $clip); $pen.Dispose(); $clip.Dispose()
        $g.Dispose()
        $file = if ($round) { 'ic_launcher_round.png' } else { 'ic_launcher.png' }
        Save-Png $bmp (Join-Path $androidRes "mipmap-$d\$file")
    }
    $tile.Dispose()
}
foreach ($st in 'disconnected', 'ready', 'streaming', 'paused', 'error') {
    Save-Png (Render-Icon 192 $st) (Join-Path $androidRes "drawable-nodpi\status_$st.png")
}
# In-app headers and the splash: the mark at 512 px with transparency, so the phone never scales the
# launcher PNG (which carries the hills tile) down into a header.
Save-Png (Render-Icon 512 'ready') (Join-Path $androidRes 'drawable-nodpi\app_logo.png')

# --- Android vector drawables ---------------------------------------------------------------------
function F([double]$v) { $v.ToString('0.##', [System.Globalization.CultureInfo]::InvariantCulture) }
function RR([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    "M$(F ($x + $r)),$(F $y)h$(F ($w - 2 * $r))a$(F $r),$(F $r) 0,0 1,$(F $r),$(F $r)v$(F ($h - 2 * $r))a$(F $r),$(F $r) 0,0 1,-$(F $r),$(F $r)h-$(F ($w - 2 * $r))a$(F $r),$(F $r) 0,0 1,-$(F $r),-$(F $r)v-$(F ($h - 2 * $r))a$(F $r),$(F $r) 0,0 1,$(F $r),-$(F $r)z"
}
function CI([double]$cx, [double]$cy, [double]$r) {
    "M$(F ($cx - $r)),$(F $cy)a$(F $r),$(F $r) 0,1 1,$(F (2 * $r)),0a$(F $r),$(F $r) 0,1 1,-$(F (2 * $r)),0z"
}
function Grad([double]$y1,[double]$y2, [string]$c1, [string]$c2) {
@"
        <aapt:attr name="android:fillColor">
            <gradient android:type="linear" android:startX="0" android:startY="$(F $y1)" android:endX="0" android:endY="$(F $y2)">
                <item android:offset="0" android:color="#FF$c1" />
                <item android:offset="1" android:color="#FF$c2" />
            </gradient>
        </aapt:attr>
"@ + "`n"
}
function Cloud([double]$x, [double]$y, [double]$s) {
    # A small cumulus: puffs on a flat rounded base, shaded underside.
    $p = (CI ($x - 6 * $s) ($y - 2.5 * $s) (4 * $s)) + (CI ($x) ($y - 5 * $s) (5.5 * $s)) + (CI ($x + 6.5 * $s) ($y - 2.8 * $s) (4.2 * $s)) + (RR ($x - 10 * $s) ($y - 3 * $s) (21 * $s) (4 * $s) (2 * $s))
@"
    <path android:fillColor="#FFD2DEEC" android:pathData="$(CI ($x - 6 * $s + 0.5 * $s) ($y - 2.5 * $s + 0.7 * $s) (4 * $s))$(CI ($x + 0.6 * $s) ($y - 5 * $s + 0.8 * $s) (5.5 * $s))$(CI ($x + 7 * $s) ($y - 2.8 * $s + 0.7 * $s) (4.2 * $s))$(RR ($x - 9.6 * $s) ($y - 2.4 * $s) (21 * $s) (4 * $s) (2 * $s))" />
    <path android:fillColor="#FFFFFFFF" android:pathData="$p" />
"@ + "`n"
}

$bg = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- Adaptive icon background: MyCam's sunny hills under a blue sky (generated by design/tools/make_icons.ps1). -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    xmlns:aapt="http://schemas.android.com/aapt"
    android:width="108dp" android:height="108dp"
    android:viewportWidth="108" android:viewportHeight="108">
    <path android:pathData="M0,0h108v108h-108z">
$(Grad 0 66 '2E7FE0' '9CC8F2')    </path>
$(Cloud 27 30 0.9)$(Cloud 84 22 0.7)$(Cloud 96 52 0.55)    <path android:pathData="M-2,70 C14,62 30,61 44,66 C52,69 58,74 64,80 L64,108 L-2,108 Z">
$(Grad 62 90 'B4E25A' '7CC63A')    </path>
    <path android:pathData="M14,108 C30,86 50,68 70,65 C84,63 98,67 110,73 L110,108 Z">
$(Grad 64 100 '9BD640' '3E9A22')    </path>
    <path android:pathData="M-2,78 C14,74 30,79 46,90 C54,96 60,102 66,110 L-2,110 Z">
$(Grad 74 108 '8ED23C' '2F8A1F')    </path>
    <path android:pathData="M58,110 C70,98 86,93 110,95 L110,110 Z">
$(Grad 93 108 '7CC836' '2F8A1F')    </path>
</vector>
"@
Write-Text (Join-Path $androidRes 'drawable\ic_launcher_background.xml') $bg

# Themed icon (Android 13+) and notification icon: white silhouettes of the *same* mark. Both are built
# from Mark-Silhouette, which fits Camera-Geometry - the geometry the colour logo is drawn from - into the
# viewport, so the body proportions, the corner radius and the lens position can never drift from the
# icon family. The themed icon is fitted to the 66 dp safe circle; the notification icon fills its 24 dp
# box, as Android's own system icons do.
function Mark-Xml([string]$comment, [double]$viewport, [string]$mode, [double]$value, [bool]$withLight) {
    $m = Mark-Silhouette $viewport $mode $value
    $v = F $viewport
    $light = if ($withLight) { "`n    <!-- The green light. -->`n    <path android:fillColor=`"#FFFFFFFF`" android:pathData=`"$(Path-To-PathData $m.light)`" />" } else { '' }
@"
<?xml version="1.0" encoding="utf-8"?>
<!-- $comment (generated by design/tools/make_icons.ps1). -->
<vector xmlns:android="http://schemas.android.com/apk/res/android"
    android:width="${v}dp" android:height="${v}dp"
    android:viewportWidth="$v" android:viewportHeight="$v">
    <!-- Shutter button. -->
    <path android:fillColor="#FFFFFFFF" android:pathData="$(Path-To-PathData $m.shutter)" />
    <!-- Body, with the lens opening cut out. -->
    <path android:fillColor="#FFFFFFFF" android:fillType="evenOdd"
        android:pathData="$(Path-To-PathData $m.body)" />
    <!-- Bezel ring. -->
    <path android:fillColor="#FFFFFFFF" android:fillType="evenOdd"
        android:pathData="$(Path-To-PathData $m.bezel)" />
    <!-- The hill inside the lens. -->
    <group>
        <clip-path android:pathData="$(Path-To-PathData $m.glass)" />
        <path android:fillColor="#FFFFFFFF" android:pathData="$(Path-To-PathData $m.hill)" />
    </group>$light
</vector>
"@
}
Write-Text (Join-Path $androidRes 'drawable\ic_launcher_monochrome.xml') (Mark-Xml 'Themed-icon (Android 13+) silhouette of the MyCam camera' 108 'radius' 33 $false)
Write-Text (Join-Path $androidRes 'drawable\ic_stat_webcam.xml') (Mark-Xml 'Notification icon: the MyCam camera as a white silhouette' 24 'width' 21 $true)

$adaptive = @"
<?xml version="1.0" encoding="utf-8"?>
<!-- XP "Snap" camera on the sunny hills (art: design/tools/make_icons.ps1). -->
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@drawable/ic_launcher_background" />
    <foreground android:drawable="@mipmap/ic_launcher_foreground" />
    <monochrome android:drawable="@drawable/ic_launcher_monochrome" />
</adaptive-icon>
"@
Write-Text (Join-Path $androidRes 'mipmap-anydpi-v26\ic_launcher.xml') $adaptive
Write-Text (Join-Path $androidRes 'mipmap-anydpi-v26\ic_launcher_round.xml') $adaptive
Write-Host 'wrote Android launcher, vector and status icons'

# --- Preview sheet --------------------------------------------------------------------------------
# Top: every tray / app state at every size. Bottom: every surface the mark appears on, side by side,
# so the audit in docs/redesign-v2.md section 11 is one look at one picture.
function Zoom($bmp, [int]$f) {
    $o, $g = New-Canvas ($bmp.Width * $f) ($bmp.Height * $f)
    $g.InterpolationMode = 'NearestNeighbor'; $g.PixelOffsetMode = 'Half'
    $g.DrawImage($bmp, 0, 0, $bmp.Width * $f, $bmp.Height * $f); $g.Dispose()
    $o
}
function Mask-Tile($bmp, [string]$shape) {
    $s = $bmp.Width
    $o, $g = New-Canvas $s $s
    $clip = switch ($shape) {
        'circle' { Circle-Path ($s / 2) ($s / 2) ($s / 2 - 0.5) }
        'squircle' { RoundRect-Path 0 0 $s $s ($s * 0.33) }
        default { RoundRect-Path 0 0 $s $s ($s * 0.17) }
    }
    $tb = New-Object System.Drawing.TextureBrush($bmp)
    $g.FillPath($tb, $clip); $tb.Dispose(); $clip.Dispose(); $g.Dispose()
    $o
}
function Fit-Tile($bmp, [int]$s, $back = $null) {
    $o, $g = New-Canvas $s $s
    if ($back) { $g.Clear($back) }
    $g.DrawImage($bmp, 0, 0, $s, $s); $g.Dispose()
    $o
}
function Silhouette-Tile([int]$s, [string]$mode, [double]$value, [bool]$light, $back) {
    $o, $g = New-Canvas $s $s
    $g.Clear($back)
    Draw-MarkSilhouette $g (Mark-Silhouette $s $mode ($value * $s)) (Hex '#FFFFFF') $light
    $g.Dispose()
    $o
}

$surfaces = New-Object System.Collections.ArrayList
function Add-Surface([string]$label, $bmp) { [void]$surfaces.Add(@{ label = $label; bmp = $bmp }) }

Add-Surface 'tray 16 px (x6)' (Zoom (Render-Icon 16 'ready') 6)
Add-Surface 'tray 24 px (x4)' (Zoom (Render-Icon 24 'streaming') 4)
Add-Surface 'tray 32 px (x3)' (Zoom (Render-Icon 32 'paused') 3)
Add-Surface 'caption / app.ico 16 (x6)' (Zoom (Render-Icon 16 'ready') 6)
Add-Surface 'PC status area 48 (x2)' (Zoom (Render-Icon 48 'ready') 2)
Add-Surface 'camera picture mark 34 (x3)' (Zoom (Render-Icon 34 'ready') 3)
Add-Surface 'installer header 55 (x2)' (Zoom (Fit-Tile (Render-Icon 55 'ready') 55 (Hex '#FFFFFF')) 2)
Add-Surface 'phone status_ready 192' (Fit-Tile (Render-Icon 192 'ready') 144)

# Adaptive icon: the real foreground over the real background, through the three launcher masks.
$adaptiveBg = Paint-Hills 216 216 -horizon 0.62
$adaptiveFg = (Adaptive-Foreground 2)[0]
$whole, $wg = New-Canvas 216 216
$wg.DrawImageUnscaled($adaptiveBg, 0, 0); $wg.DrawImageUnscaled($adaptiveFg, 0, 0); $wg.Dispose()
$visible = $whole.Clone((New-Object System.Drawing.Rectangle 36, 36, 144, 144), $whole.PixelFormat)
$visible144 = Fit-Tile $visible 144
foreach ($shape in 'circle', 'squircle', 'square') {
    Add-Surface "adaptive, $shape mask" (Mask-Tile $visible144 $shape)
}
$adaptiveBg.Dispose(); $adaptiveFg.Dispose(); $whole.Dispose(); $visible.Dispose(); $visible144.Dispose()

$legacy = [System.Drawing.Image]::FromFile((Join-Path $androidRes 'mipmap-xxxhdpi\ic_launcher.png'))
$roundIcon = [System.Drawing.Image]::FromFile((Join-Path $androidRes 'mipmap-xxxhdpi\ic_launcher_round.png'))
Add-Surface 'legacy launcher (API < 26)' (Fit-Tile $legacy 144)
Add-Surface 'legacy round' (Fit-Tile $roundIcon 144)
$legacy.Dispose(); $roundIcon.Dispose()
Add-Surface 'themed icon (mono)' (Mask-Tile (Silhouette-Tile 144 'radius' (33 / 108) $false (Hex '#4A5568')) 'circle')
Add-Surface 'notification 24 (x6)' (Zoom (Silhouette-Tile 24 'width' (21 / 24) $true (Hex '#202124')) 6)
Add-Surface 'notification, large' (Silhouette-Tile 144 'width' (21 / 24) $true (Hex '#202124'))
Add-Surface 'app_logo.png (in-app, splash)' (Fit-Tile (Render-Icon 512 'ready') 144)
$storeIcon = Join-Path $assetDir 'store\play-icon-512.png'
if (Test-Path $storeIcon) {
    $si = [System.Drawing.Image]::FromFile($storeIcon)
    Add-Surface 'Play icon 512' (Mask-Tile (Fit-Tile $si 144) 'square')
    $si.Dispose()
}

$rowH = 300; $labelW = 140
$cell = 170; $cols = 7
$surfaceRows = [Math]::Ceiling($surfaces.Count / [double]$cols)
$sheet = New-Object System.Drawing.Bitmap ($labelW + 1100), ($rowH * $states.Count + 40 + $surfaceRows * ($cell + 36))
$g = [System.Drawing.Graphics]::FromImage($sheet)
Set-Quality $g
$g.Clear((Hex '#ECE9D8'))
$font = New-Object System.Drawing.Font('Tahoma', 13, [System.Drawing.GraphicsUnit]::Pixel)
$ink = New-Object System.Drawing.SolidBrush((C 255 30 30 30))
for ($row = 0; $row -lt $states.Count; $row++) {
    $st = $states[$row]; $y0 = $row * $rowH + 10
    $g.DrawString($st, $font, $ink, 10, $y0 + 120)
    $x = $labelW
    foreach ($b in $rendered[$st]) { if ($b.Width -eq 256) { $g.DrawImage($b, $x, $y0) } }
    $x += 270
    foreach ($b in $rendered[$st]) { if ($b.Width -lt 256) { $g.DrawImage($b, $x, $y0 + 200 - $b.Height); $x += $b.Width + 10 } }
    $g.InterpolationMode = 'NearestNeighbor'; $g.PixelOffsetMode = 'Half'
    $x = $labelW + 270
    foreach ($b in $rendered[$st]) { if ($b.Width -in 16, 24, 32) { $g.DrawImage($b, $x, $y0, $b.Width * 4, $b.Height * 4); $x += $b.Width * 4 + 16 } }
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
}
# Every surface, side by side (docs/redesign-v2.md section 11).
$titleFont = New-Object System.Drawing.Font('Trebuchet MS', 20, [System.Drawing.FontStyle]::Bold, [System.Drawing.GraphicsUnit]::Pixel)
$y0 = $rowH * $states.Count
$g.DrawString('Every surface the mark appears on', $titleFont, $ink, 10, $y0 + 6)
$titleFont.Dispose()
$y0 += 36
for ($i = 0; $i -lt $surfaces.Count; $i++) {
    $col = $i % $cols; $row = [Math]::Floor($i / $cols)
    $x = 10 + $col * $cell; $y = $y0 + $row * ($cell + 36)
    $b = $surfaces[$i].bmp
    $g.DrawImage($b, [int]($x + ($cell - 20 - $b.Width) / 2), $y, $b.Width, $b.Height)
    $g.DrawString($surfaces[$i].label, $font, $ink, (RectF $x ($y + $cell + 2) ($cell - 10) 32))
    $b.Dispose()
}
$g.Dispose()
$sheet.Save((Join-Path $previewDir 'icons.png'), [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host 'wrote design\previews\icons.png'
