<#
  Shared GDI+ drawing for MyCam's Windows XP (Luna) artwork. Dot-source it:  . "$PSScriptRoot\luna_draw.ps1"
  Used by make_icons.ps1, make_status_frames.ps1, make_installer_art.ps1 and make_store_art.ps1.

  Contents
    LunaFx (inline C#)   blur (box x3 ~ Gaussian), noise, desaturate, alpha compositing on locked bits
    Draw-Camera          the "Snap" logo: a friendly compact camera whose lens shows a tiny landscape
    Draw-BadgeAt         glossy XP status badges (streaming / paused / error)
    Render-Icon          logo + shadow + badge (or desaturated) at any size
    Paint-Hills          the original procedural "sunny hills" scene (sky, cumulus, rolling hills)
    Draw-FrostedCard     frosted glass card over a scene
    Draw-ProgressTrack   the empty XP progress-bar track

  Everything here is drawn from scratch. Nothing is traced from or embeds a photo or Microsoft artwork.
  Keep this file ASCII (Windows PowerShell 5 misreads BOM-less UTF-8).
#>
Add-Type -AssemblyName System.Drawing

if (-not ('LunaFx' -as [type])) {
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class LunaFx {
    static int[] Read(Bitmap b) {
        BitmapData d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        int[] px = new int[b.Width * b.Height];
        Marshal.Copy(d.Scan0, px, 0, px.Length);
        b.UnlockBits(d);
        return px;
    }
    static void Write(Bitmap b, int[] px) {
        BitmapData d = b.LockBits(new Rectangle(0, 0, b.Width, b.Height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        Marshal.Copy(px, 0, d.Scan0, px.Length);
        b.UnlockBits(d);
    }
    static int Clamp(double v) { return v < 0 ? 0 : (v > 255 ? 255 : (int)(v + 0.5)); }
    static int Pack(int a, int r, int g, int b) { return (a << 24) | (r << 16) | (g << 8) | b; }

    // Box sizes whose three passes approximate a Gaussian of the given sigma.
    static int[] BoxesForGauss(double sigma, int n) {
        double wIdeal = Math.Sqrt((12 * sigma * sigma / n) + 1);
        int wl = (int)Math.Floor(wIdeal); if (wl % 2 == 0) wl--;
        int wu = wl + 2;
        double mIdeal = (12 * sigma * sigma - n * wl * wl - 4 * n * wl - 3 * n) / (-4.0 * wl - 4);
        int m = (int)Math.Round(mIdeal);
        int[] s = new int[n];
        for (int i = 0; i < n; i++) s[i] = i < m ? wl : wu;
        return s;
    }
    static void BoxH(float[] s, float[] t, int w, int h, int r) {
        float k = 1f / (2 * r + 1);
        for (int y = 0; y < h; y++) {
            int o = y * w; float v = 0;
            for (int i = -r; i <= r; i++) v += s[o + Math.Min(Math.Max(i, 0), w - 1)];
            for (int x = 0; x < w; x++) {
                t[o + x] = v * k;
                v += s[o + Math.Min(x + r + 1, w - 1)] - s[o + Math.Max(x - r, 0)];
            }
        }
    }
    static void BoxV(float[] s, float[] t, int w, int h, int r) {
        float k = 1f / (2 * r + 1);
        for (int x = 0; x < w; x++) {
            float v = 0;
            for (int i = -r; i <= r; i++) v += s[Math.Min(Math.Max(i, 0), h - 1) * w + x];
            for (int y = 0; y < h; y++) {
                t[y * w + x] = v * k;
                v += s[Math.Min(y + r + 1, h - 1) * w + x] - s[Math.Max(y - r, 0) * w + x];
            }
        }
    }

    // Gaussian-like blur (three box passes) in premultiplied space, so transparent edges don't go dark.
    public static void Blur(Bitmap b, double sigma) {
        if (sigma < 0.4) return;
        int w = b.Width, h = b.Height, n = w * h;
        int[] px = Read(b);
        float[][] ch = new float[4][];
        for (int c = 0; c < 4; c++) ch[c] = new float[n];
        for (int i = 0; i < n; i++) {
            int p = px[i]; float a = (p >> 24) & 255; float f = a / 255f;
            ch[0][i] = a; ch[1][i] = ((p >> 16) & 255) * f; ch[2][i] = ((p >> 8) & 255) * f; ch[3][i] = (p & 255) * f;
        }
        int[] boxes = BoxesForGauss(sigma, 3);
        float[] tmp = new float[n];
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < 3; k++) { int r = (boxes[k] - 1) / 2; BoxH(ch[c], tmp, w, h, r); BoxV(tmp, ch[c], w, h, r); }
        for (int i = 0; i < n; i++) {
            float a = ch[0][i];
            if (a < 0.5f) { px[i] = 0; continue; }
            float f = 255f / a;
            px[i] = Pack(Clamp(a), Clamp(ch[1][i] * f), Clamp(ch[2][i] * f), Clamp(ch[3][i] * f));
        }
        Write(b, px);
    }

    // Smooth value noise in [-1, 1] on a grid of cell size (sx, sy).
    static float[] Grid(int gw, int gh, Random rnd) {
        float[] g = new float[gw * gh];
        for (int i = 0; i < g.Length; i++) g[i] = (float)(rnd.NextDouble() * 2 - 1);
        return g;
    }
    static float Sample(float[] g, int gw, float gx, float gy) {
        int x0 = (int)gx, y0 = (int)gy; float fx = gx - x0, fy = gy - y0;
        fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
        float a = g[y0 * gw + x0], b = g[y0 * gw + x0 + 1], c = g[(y0 + 1) * gw + x0], d = g[(y0 + 1) * gw + x0 + 1];
        return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
    }

    // Low-contrast "grass" texture on non-transparent pixels: fine grain plus stretched blotches.
    public static void Noise(Bitmap b, int seed, float fine, float coarse, float sx, float sy) {
        int w = b.Width, h = b.Height;
        int[] px = Read(b);
        Random rnd = new Random(seed);
        int gw1 = (int)(w / sx) + 3, gh1 = (int)(h / sy) + 3;
        int gw2 = (int)(w / (sx * 0.35f)) + 3, gh2 = (int)(h / (sy * 0.35f)) + 3;
        float[] g1 = Grid(gw1, gh1, rnd), g2 = Grid(gw2, gh2, rnd);
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            int i = y * w + x, p = px[i];
            int a = (p >> 24) & 255; if (a == 0) continue;
            float n = Sample(g1, gw1, x / sx, y / sy) * 0.65f + Sample(g2, gw2, x / (sx * 0.35f), y / (sy * 0.35f)) * 0.35f;
            float d = coarse * n + fine * (float)(rnd.NextDouble() * 2 - 1);
            px[i] = Pack(a, Clamp(((p >> 16) & 255) + d * 0.8f), Clamp(((p >> 8) & 255) + d), Clamp((p & 255) + d * 0.5f));
        }
        Write(b, px);
    }

    // Mix toward grey by `amount`, then toward white by `lift`.
    public static void Desaturate(Bitmap b, float amount, float lift) {
        int[] px = Read(b);
        for (int i = 0; i < px.Length; i++) {
            int p = px[i]; int a = (p >> 24) & 255; if (a == 0) continue;
            float r = (p >> 16) & 255, g = (p >> 8) & 255, bl = p & 255;
            float l = 0.3f * r + 0.59f * g + 0.11f * bl;
            r += (l - r) * amount; g += (l - g) * amount; bl += (l - bl) * amount;
            r += (255 - r) * lift; g += (255 - g) * lift; bl += (255 - bl) * lift;
            px[i] = Pack(a, Clamp(r), Clamp(g), Clamp(bl));
        }
        Write(b, px);
    }

    // "Not connected": grey, a touch cooler, flatter and lighter, slightly see-through.
    public static void Inactive(Bitmap b) {
        int[] px = Read(b);
        for (int i = 0; i < px.Length; i++) {
            int p = px[i]; int a = (p >> 24) & 255; if (a == 0) continue;
            float l = 0.3f * ((p >> 16) & 255) + 0.59f * ((p >> 8) & 255) + 0.11f * (p & 255);
            l = 150 + (l - 128) * 0.8f;
            px[i] = Pack(Clamp(a * 0.9), Clamp(l - 4), Clamp(l - 1), Clamp(l + 6));
        }
        Write(b, px);
    }

    // src over dst, keeping dst's alpha ("source atop"): paints only where dst already has pixels.
    public static void Atop(Bitmap dst, Bitmap src, float opacity) {
        int[] d = Read(dst), s = Read(src);
        for (int i = 0; i < d.Length; i++) {
            int q = s[i]; float sa = ((q >> 24) & 255) / 255f * opacity; if (sa <= 0) continue;
            int p = d[i];
            int r = Clamp(((p >> 16) & 255) * (1 - sa) + ((q >> 16) & 255) * sa);
            int g = Clamp(((p >> 8) & 255) * (1 - sa) + ((q >> 8) & 255) * sa);
            int bl = Clamp((p & 255) * (1 - sa) + (q & 255) * sa);
            d[i] = (p & unchecked((int)0xFF000000)) | (r << 16) | (g << 8) | bl;
        }
        Write(dst, d);
    }

    // Straight-alpha src over dst with an opacity.
    public static void Over(Bitmap dst, Bitmap src, float opacity) {
        int[] d = Read(dst), s = Read(src);
        for (int i = 0; i < d.Length; i++) {
            int q = s[i]; float sa = ((q >> 24) & 255) / 255f * opacity; if (sa <= 0) continue;
            int p = d[i]; float da = ((p >> 24) & 255) / 255f;
            float oa = sa + da * (1 - sa);
            float r = (((q >> 16) & 255) * sa + ((p >> 16) & 255) * da * (1 - sa)) / oa;
            float g = (((q >> 8) & 255) * sa + ((p >> 8) & 255) * da * (1 - sa)) / oa;
            float bl = ((q & 255) * sa + (p & 255) * da * (1 - sa)) / oa;
            d[i] = Pack(Clamp(oa * 255), Clamp(r), Clamp(g), Clamp(bl));
        }
        Write(dst, d);
    }

    // A silhouette of src's alpha in one colour (for drop shadows).
    public static Bitmap Silhouette(Bitmap src, Color c, float alpha) {
        int[] s = Read(src);
        for (int i = 0; i < s.Length; i++) {
            int a = Clamp(((s[i] >> 24) & 255) * alpha * c.A / 255f);
            s[i] = Pack(a, c.R, c.G, c.B);
        }
        Bitmap b = new Bitmap(src.Width, src.Height, PixelFormat.Format32bppArgb);
        Write(b, s);
        return b;
    }

    // Multiply dst's alpha by the inverse of mask's alpha (cut a shape out of a layer).
    public static void CutAlpha(Bitmap dst, Bitmap mask) {
        int[] d = Read(dst), m = Read(mask);
        for (int i = 0; i < d.Length; i++) {
            int a = ((d[i] >> 24) & 255) * (255 - ((m[i] >> 24) & 255)) / 255;
            d[i] = (d[i] & 0xFFFFFF) | (a << 24);
        }
        Write(dst, d);
    }

    // Multiply the colour of every pixel by `f` (darkens a backdrop without touching alpha).
    public static void Scale(Bitmap b, float f) {
        int[] px = Read(b);
        for (int i = 0; i < px.Length; i++) {
            int p = px[i];
            px[i] = Pack((p >> 24) & 255, Clamp(((p >> 16) & 255) * f), Clamp(((p >> 8) & 255) * f), Clamp((p & 255) * f));
        }
        Write(b, px);
    }

    // The bounding box of the pixels whose alpha is above `minAlpha`, as x, y, w, h (w = 0 if empty).
    public static int[] AlphaBounds(Bitmap b, int minAlpha) {
        int w = b.Width, h = b.Height;
        int[] px = Read(b);
        int x0 = w, y0 = h, x1 = -1, y1 = -1;
        for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
            if (((px[y * w + x] >> 24) & 255) <= minAlpha) continue;
            if (x < x0) x0 = x; if (x > x1) x1 = x;
            if (y < y0) y0 = y; if (y > y1) y1 = y;
        }
        if (x1 < 0) return new int[] { 0, 0, 0, 0 };
        return new int[] { x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
    }

    // Multiply src's alpha by mask's alpha (clip a soft layer to a shape).
    public static void MaskAlpha(Bitmap src, Bitmap mask) {
        int[] s = Read(src), m = Read(mask);
        for (int i = 0; i < s.Length; i++) {
            int a = ((s[i] >> 24) & 255) * ((m[i] >> 24) & 255) / 255;
            s[i] = (s[i] & 0xFFFFFF) | (a << 24);
        }
        Write(src, s);
    }
}
'@
}

function C([int]$a, [int]$r, [int]$g, [int]$b) { [System.Drawing.Color]::FromArgb($a, $r, $g, $b) }
function Hex([string]$h, [int]$a = 255) {
    $h = $h.TrimStart('#')
    [System.Drawing.Color]::FromArgb($a, [Convert]::ToInt32($h.Substring(0, 2), 16), [Convert]::ToInt32($h.Substring(2, 2), 16), [Convert]::ToInt32($h.Substring(4, 2), 16))
}
function PtF([double]$x, [double]$y) { New-Object System.Drawing.PointF([single]$x, [single]$y) }
function RectF([double]$x, [double]$y, [double]$w, [double]$h) { New-Object System.Drawing.RectangleF([single]$x, [single]$y, [single]$w, [single]$h) }

function Set-Quality($g) {
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.CompositingQuality = 'HighQuality'; $g.TextRenderingHint = 'AntiAliasGridFit'
}

function New-Canvas([int]$w, [int]$h, [bool]$alpha = $true) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    Set-Quality $g
    if ($alpha) { $g.Clear([System.Drawing.Color]::Transparent) } else { $g.Clear([System.Drawing.Color]::White) }
    @($bmp, $g)
}

# 24-bit copy, for outputs that must not carry alpha (BMPs, the camera frames, Play's feature graphic).
function To-Rgb24([System.Drawing.Bitmap]$bmp) {
    $out = New-Object System.Drawing.Bitmap $bmp.Width, $bmp.Height, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($out)
    $g.Clear([System.Drawing.Color]::White); $g.DrawImageUnscaled($bmp, 0, 0); $g.Dispose()
    $out
}

function Copy-Bitmap([System.Drawing.Bitmap]$bmp) {
    $out = New-Object System.Drawing.Bitmap $bmp.Width, $bmp.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($out); $g.DrawImageUnscaled($bmp, 0, 0); $g.Dispose()
    $out
}

function RoundRect-Path([double]$x, [double]$y, [double]$w, [double]$h, [double]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $r = [Math]::Min($r, [Math]::Min($w, $h) / 2)
    if ($r -le 0.01) { $p.AddRectangle((RectF $x $y $w $h)); return , $p }
    $d = 2 * $r
    $p.AddArc([single]$x, [single]$y, [single]$d, [single]$d, 180, 90)
    $p.AddArc([single]($x + $w - $d), [single]$y, [single]$d, [single]$d, 270, 90)
    $p.AddArc([single]($x + $w - $d), [single]($y + $h - $d), [single]$d, [single]$d, 0, 90)
    $p.AddArc([single]$x, [single]($y + $h - $d), [single]$d, [single]$d, 90, 90)
    $p.CloseFigure()
    , $p
}

function Circle-Path([double]$cx, [double]$cy, [double]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddEllipse([single]($cx - $r), [single]($cy - $r), [single](2 * $r), [single](2 * $r))
    , $p
}

function Linear-Brush($rect, $c1, $c2, [single]$angle = 90, [single[]]$pos = $null, $colors = $null) {
    $b = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, $c1, $c2, $angle)
    $b.WrapMode = [System.Drawing.Drawing2D.WrapMode]::TileFlipXY
    if ($colors) {
        $blend = New-Object System.Drawing.Drawing2D.ColorBlend $colors.Count
        $blend.Colors = [System.Drawing.Color[]]$colors; $blend.Positions = $pos
        $b.InterpolationColors = $blend
    }
    , $b
}

function Fill-Linear($g, $path, $rect, $c1, $c2, [single]$angle = 90) {
    $b = Linear-Brush $rect $c1 $c2 $angle
    $g.FillPath($b, $path); $b.Dispose()
}

function Fill-Solid($g, $path, $color) {
    $b = New-Object System.Drawing.SolidBrush($color); $g.FillPath($b, $path); $b.Dispose()
}

# Radial-gradient filled circle with the bright centre offset (light from the top left).
function Fill-Sphere($g, [double]$cx, [double]$cy, [double]$r, $inner, $outer, [double]$hx = -0.35, [double]$hy = -0.4) {
    $path = Circle-Path $cx $cy $r
    $brush = New-Object System.Drawing.Drawing2D.PathGradientBrush($path)
    $brush.CenterPoint = PtF ($cx + $hx * $r) ($cy + $hy * $r)
    $brush.CenterColor = $inner
    $brush.SurroundColors = [System.Drawing.Color[]]@($outer)
    $g.FillPath($brush, $path)
    $brush.Dispose(); $path.Dispose()
}

# XP gloss: a soft white ellipse over the top part of a round shape.
function Draw-Gloss($g, [double]$cx, [double]$cy, [double]$r, [int]$alpha) {
    $rect = RectF ($cx - 0.72 * $r) ($cy - 0.9 * $r) (1.44 * $r) (0.95 * $r)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush($rect, (C $alpha 255 255 255), (C 0 255 255 255), [single]90)
    $brush.WrapMode = [System.Drawing.Drawing2D.WrapMode]::TileFlipXY
    $g.FillEllipse($brush, $rect)
    $brush.Dispose()
}

# =====================================================================================================
#  The "Snap" logo
# =====================================================================================================

# Detail level for a pixel size: 0 = body + bezel + sky/hill (16-20 px), 1 = + shutter and band (24-40),
# 2 = full detail (48 px and up).
function Camera-Level([double]$s) { if ($s -lt 23) { 0 } elseif ($s -lt 46) { 1 } else { 2 } }

# Geometry of the camera in an s x s box at (ox, oy). Small sizes snap to whole pixels so edges stay crisp.
function Camera-Geometry([double]$s, [double]$ox = 0, [double]$oy = 0) {
    $lv = Camera-Level $s
    $snap = $s -lt 46
    function S([double]$v) { if ($snap) { [Math]::Round($v) } else { $v } }
    $k = @{ level = $lv; s = $s }
    if ($lv -eq 0) {
        # 16 px: the body fills the box; the lens is as large as it can be.
        $k.bx = $ox + (S (0.0 * $s)); $k.by = $oy + (S (0.19 * $s)); $k.bw = (S (1.0 * $s)); $k.bh = (S (0.75 * $s))
        $k.br = 0.2 * $k.bh
        $k.cx = $ox + 0.5 * $s; $k.cy = $k.by + $k.bh * 0.52; $k.R = 0.46 * $k.bh
    } else {
        $k.bx = $ox + (S (0.04 * $s)); $k.by = $oy + (S (0.19 * $s)); $k.bw = (S (0.89 * $s)); $k.bh = (S (0.68 * $s))
        $k.br = 0.22 * $k.bh
        $k.cx = $k.bx + $k.bw * 0.47; $k.cy = $k.by + $k.bh * 0.57; $k.R = $(if ($snap) { 0.41 } else { 0.385 }) * $k.bh
        if ($snap) { $k.cx = [Math]::Round($k.cx * 2) / 2; $k.cy = [Math]::Round($k.cy * 2) / 2 }
    }
    $k.line = [Math]::Max(1.0, $s / 64)     # outline width
    $k
}

# The landscape inside the lens: sky, a cloud and a hill (or two flat blocks at 16 px).
function Draw-LensScene($g, $k, [double]$cx, [double]$cy, [double]$r) {
    $clip = Circle-Path $cx $cy $r
    $state = $g.Save()
    $g.SetClip($clip)
    $sky = RectF ($cx - $r) ($cy - $r) (2 * $r) (2 * $r)
    if ($k.level -eq 0) {
        Fill-Linear $g $clip $sky (Hex '#5FAEFF') (Hex '#2E7FE0') 90
        $hill = New-Object System.Drawing.Drawing2D.GraphicsPath
        $hill.AddRectangle((RectF ($cx - $r) ($cy + 0.02 * $r) (2 * $r) (2 * $r)))
        Fill-Solid $g $hill (Hex '#4FB52A')
        $hill.Dispose()
    } else {
        $bb = Linear-Brush $sky (Hex '#2E7FE0') (Hex '#B5DBFF') 90 ([single[]]@(0, 0.55, 1)) @((Hex '#2E7FE0'), (Hex '#8CC4F7'), (Hex '#C9E5FF'))
        $g.FillPath($bb, $clip); $bb.Dispose()
        # Cloud (from 24 px up): three overlapping puffs, upper left of centre.
        if ($r -ge 4) {
            $wb = New-Object System.Drawing.SolidBrush((Hex '#FFFFFF' 245))
            $u = $r / 10
            $g.FillEllipse($wb, [single]($cx - 6.3 * $u), [single]($cy - 3.6 * $u), [single](4.6 * $u), [single](3.0 * $u))
            $g.FillEllipse($wb, [single]($cx - 4.6 * $u), [single]($cy - 5.4 * $u), [single](4.4 * $u), [single](4.2 * $u))
            $g.FillEllipse($wb, [single]($cx - 1.9 * $u), [single]($cy - 4.2 * $u), [single](4.0 * $u), [single](3.0 * $u))
            $wb.Dispose()
        }
        # Back hill (lighter) and front hill (deeper), as in the big scene.
        $back = New-Object System.Drawing.Drawing2D.GraphicsPath
        $back.AddBezier((PtF ($cx - 1.1 * $r) ($cy + 0.30 * $r)), (PtF ($cx - 0.5 * $r) ($cy + 0.02 * $r)), (PtF ($cx + 0.1 * $r) ($cy + 0.05 * $r)), (PtF ($cx + 0.6 * $r) ($cy + 0.45 * $r)))
        $back.AddLine((PtF ($cx + 0.6 * $r) ($cy + 0.45 * $r)), (PtF ($cx + 0.6 * $r) ($cy + 1.2 * $r)))
        $back.AddLine((PtF ($cx + 0.6 * $r) ($cy + 1.2 * $r)), (PtF ($cx - 1.1 * $r) ($cy + 1.2 * $r)))
        $back.CloseFigure()
        Fill-Linear $g $back (RectF ($cx - $r) ($cy) (2 * $r) ($r)) (Hex '#B4E25A') (Hex '#6CC234') 90
        $front = New-Object System.Drawing.Drawing2D.GraphicsPath
        $front.AddBezier((PtF ($cx - 1.1 * $r) ($cy + 0.75 * $r)), (PtF ($cx - 0.3 * $r) ($cy + 0.35 * $r)), (PtF ($cx + 0.45 * $r) ($cy + 0.05 * $r)), (PtF ($cx + 1.1 * $r) ($cy + 0.2 * $r)))
        $front.AddLine((PtF ($cx + 1.1 * $r) ($cy + 0.2 * $r)), (PtF ($cx + 1.1 * $r) ($cy + 1.2 * $r)))
        $front.AddLine((PtF ($cx + 1.1 * $r) ($cy + 1.2 * $r)), (PtF ($cx - 1.1 * $r) ($cy + 1.2 * $r)))
        $front.CloseFigure()
        Fill-Linear $g $front (RectF ($cx - $r) ($cy + 0.1 * $r) (2 * $r) (0.9 * $r)) (Hex '#5DB82E') (Hex '#2F8A1F') 90
        $back.Dispose(); $front.Dispose()
    }
    # Soft inner shadow at the rim gives the glass depth (not at 16 px).
    if ($k.level -ge 1 -and $k.s -ge 30) {
        $pg = New-Object System.Drawing.Drawing2D.PathGradientBrush($clip)
        $pg.CenterColor = (C 0 0 20 60); $pg.SurroundColors = [System.Drawing.Color[]]@((C 110 0 20 60))
        $bl = New-Object System.Drawing.Drawing2D.Blend 3
        $bl.Factors = [single[]]@(1, 0, 0); $bl.Positions = [single[]]@(0, 0.28, 1)
        $pg.Blend = $bl
        $g.FillPath($pg, $clip); $pg.Dispose()
    }
    $g.Restore($state)
    # Highlight arc along the upper-left rim of the glass (never a dot in the centre).
    if ($k.level -ge 1) {
        $pw = [Math]::Max(1.0, $r * 0.14)
        $ar = $r - $pw * 0.5 - [Math]::Max(0.5, $r * 0.06)
        $pen = New-Object System.Drawing.Pen((C 210 255 255 255), [single]$pw)
        $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
        $g.DrawArc($pen, [single]($cx - $ar), [single]($cy - $ar), [single](2 * $ar), [single](2 * $ar), 196, 62)
        $pen.Dispose()
    }
    $clip.Dispose()
}

# Silver ring: dark edge, then a gradient disc. `reverse` flips the light for a recessed bevel.
function Draw-Ring($g, [double]$cx, [double]$cy, [double]$r, [double]$edge, [bool]$reverse, $edgeColor) {
    $p = Circle-Path $cx $cy $r
    Fill-Solid $g $p $edgeColor; $p.Dispose()
    $ri = $r - $edge
    $p = Circle-Path $cx $cy $ri
    $rect = RectF ($cx - $ri) ($cy - $ri) (2 * $ri) (2 * $ri)
    if ($reverse) { $b = Linear-Brush $rect (Hex '#7E8A99') (Hex '#F7F9FB') 45 }
    else { $b = Linear-Brush $rect (Hex '#FFFFFF') (Hex '#8F9AA8') 45 ([single[]]@(0, 0.45, 1)) @((Hex '#FFFFFF'), (Hex '#D9DFE6'), (Hex '#8996A6')) }
    $g.FillPath($b, $p); $b.Dispose(); $p.Dispose()
}

# The camera itself (no shadow, no badge). s x s box at (ox, oy).
function Draw-Camera($g, [double]$s, [double]$ox = 0, [double]$oy = 0) {
    $k = Camera-Geometry $s $ox $oy
    $lv = $k.level; $ln = $k.line
    $bx = $k.bx; $by = $k.by; $bw = $k.bw; $bh = $k.bh; $br = $k.br

    # Amber shutter button on top (24 px and up), drawn first so the body overlaps its base.
    if ($lv -ge 1) {
        if ($lv -eq 1) {
            $sw = [Math]::Round($bw * 0.24); $sh = [Math]::Max(3, [Math]::Round($s * 0.12))
            $sx = [Math]::Round($bx + $bw * 0.66); $sy = $by - $sh + [Math]::Max(1, [Math]::Round($s * 0.04))
        } else {
            $sw = $bw * 0.22; $sh = $s * 0.1; $sx = $bx + $bw * 0.66; $sy = $by - $s * 0.065
        }
        $sp = RoundRect-Path $sx $sy $sw $sh ([Math]::Max(1, $sh * 0.35))
        Fill-Solid $g $sp (Hex '#8A4E00'); $sp.Dispose()
        $sp = RoundRect-Path ($sx + $ln) ($sy + $ln) ($sw - 2 * $ln) ($sh - $ln) ([Math]::Max(0.5, $sh * 0.3))
        Fill-Linear $g $sp (RectF $sx $sy $sw ($sh * 0.8)) (Hex '#FFE08A') (Hex '#E08A00') 90; $sp.Dispose()
        if ($lv -ge 2) {
            $hp = RoundRect-Path ($sx + $sw * 0.18) ($sy + $sh * 0.16) ($sw * 0.64) ($sh * 0.24) ($sh * 0.12)
            Fill-Solid $g $hp (C 170 255 255 255); $hp.Dispose()
        }
    }

    # 3/4 depth: a darker slab peeking out right and below (5 deg yaw feel). Not at 16 px.
    if ($lv -ge 2) {
        $dp = RoundRect-Path ($bx + $bw * 0.03) ($by + $bh * 0.04) $bw $bh $br
        Fill-Linear $g $dp (RectF $bx $by $bw $bh) (Hex '#1B4FAE') (Hex '#0B2C78') 0; $dp.Dispose()
    }

    # Body: dark outline, then the Luna-blue fill.
    $outer = RoundRect-Path $bx $by $bw $bh $br
    Fill-Solid $g $outer (Hex '#0B3487')
    $inner = RoundRect-Path ($bx + $ln) ($by + $ln) ($bw - 2 * $ln) ($bh - 2 * $ln) ([Math]::Max(0.5, $br - $ln))
    Fill-Linear $g $inner (RectF $bx $by $bw $bh) (Hex '#3A86F0') (Hex '#1A4FB8') 90
    # Side light: a little brighter on the left, a little darker on the right.
    $side = Linear-Brush (RectF $bx $by $bw $bh) (C 0 0 0 0) (C 0 0 0 0) 0 ([single[]]@(0, 0.25, 0.7, 1)) @((C 40 255 255 255), (C 0 255 255 255), (C 0 0 20 60), (C 60 0 20 60))
    $g.FillPath($side, $inner); $side.Dispose()

    $state = $g.Save()
    $g.SetClip($inner)
    if ($lv -ge 1) {
        # Silver top band with XP gloss.
        $bandH = if ($lv -eq 1) { [Math]::Max(2, [Math]::Round($bh * 0.26)) } else { $bh * 0.27 }
        $band = New-Object System.Drawing.Drawing2D.GraphicsPath
        $band.AddRectangle((RectF $bx $by $bw $bandH))
        $bb = Linear-Brush (RectF $bx $by $bw $bandH) (Hex '#F4F6F8') (Hex '#C9D0D8') 90 ([single[]]@(0, 0.5, 1)) @((Hex '#FFFFFF'), (Hex '#E6EAEE'), (Hex '#BFC7D1'))
        $g.FillPath($bb, $band); $bb.Dispose(); $band.Dispose()
        $lineB = New-Object System.Drawing.SolidBrush((Hex '#5A6E8C'))
        $g.FillRectangle($lineB, [single]$bx, [single]($by + $bandH), [single]$bw, [single]([Math]::Max(1, $ln * 0.8)))
        $lineB.Dispose()
        # Soft gloss on the blue just under the band.
        $gl = RectF $bx ($by + $bandH) $bw ($bh * 0.3)
        $gb = New-Object System.Drawing.Drawing2D.LinearGradientBrush($gl, (C 70 255 255 255), (C 0 255 255 255), [single]90)
        $gb.WrapMode = [System.Drawing.Drawing2D.WrapMode]::TileFlipXY
        $g.FillRectangle($gb, $gl.X, ($gl.Y + 1), $gl.Width, $gl.Height); $gb.Dispose()
    } else {
        # 16-20 px: just a light top edge.
        $hb = New-Object System.Drawing.SolidBrush((C 120 255 255 255))
        $g.FillRectangle($hb, [single]($bx + 1), [single]($by + 1), [single]($bw - 2), [single]1); $hb.Dispose()
    }
    $g.Restore($state)

    # Viewfinder window and the green "on" light (full detail only).
    if ($lv -ge 2) {
        $vw = $bw * 0.15; $vh = $bh * 0.13; $vx = $bx + $bw * 0.08; $vy = $by + $bh * 0.07
        $vp = RoundRect-Path $vx $vy $vw $vh ($vh * 0.25)
        Fill-Solid $g $vp (Hex '#3B4B63'); $vp.Dispose()
        $vp = RoundRect-Path ($vx + $ln) ($vy + $ln) ($vw - 2 * $ln) ($vh - 2 * $ln) ($vh * 0.2)
        Fill-Linear $g $vp (RectF $vx $vy $vw $vh) (Hex '#9CCBFA') (Hex '#1F4F94') 60; $vp.Dispose()
        $hb = New-Object System.Drawing.SolidBrush((C 150 255 255 255))
        $g.FillRectangle($hb, [single]($vx + $vw * 0.2), [single]($vy + $vh * 0.22), [single]($vw * 0.25), [single]($vh * 0.18)); $hb.Dispose()
    }
    if ($lv -ge 2 -or ($lv -eq 1 -and $s -ge 32)) {
        $lr = [Math]::Max(1.5, $bh * 0.055); $lx = $bx + $bw * 0.86; $ly = $by + $bh * 0.135
        if ($lv -eq 1) { $lr = [Math]::Max(1.5, [Math]::Round($s * 0.05 * 2) / 2) }
        $lp = Circle-Path $lx $ly ($lr + $ln * 0.7)
        Fill-Solid $g $lp (Hex '#2E5A2E' 200); $lp.Dispose()
        Fill-Sphere $g $lx $ly $lr (Hex '#C8FFA8') (Hex '#22A01E') -0.3 -0.35
    }

    # Lens: thick double silver bezel, a dark glass rim, then the landscape.
    $cx = $k.cx; $cy = $k.cy; $R = $k.R
    if ($lv -eq 0) {
        Draw-Ring $g $cx $cy $R $ln $false (Hex '#33415A')
        Draw-LensScene $g $k $cx $cy ($R - 2 * $ln)
        $p = Circle-Path $cx $cy ($R - 2 * $ln)
        $pen = New-Object System.Drawing.Pen((C 90 0 20 50), [single]0.6); $g.DrawPath($pen, $p); $pen.Dispose(); $p.Dispose()
    } else {
        if ($lv -ge 2) {
            # A soft contact shadow of the bezel on the body.
            $sp = Circle-Path ($cx + $R * 0.05) ($cy + $R * 0.07) ($R * 1.04)
            Fill-Solid $g $sp (C 60 0 15 50); $sp.Dispose()
        }
        Draw-Ring $g $cx $cy $R $ln $false (Hex '#2E3C55')
        if ($lv -ge 2) {
            $r2 = $R * 0.8
            Draw-Ring $g $cx $cy $r2 ([Math]::Max(0.8, $ln * 0.8)) $true (Hex '#7C8899')
            $r3 = $R * 0.66
        } elseif ($s -ge 30) {
            # 32-40 px: the second (recessed) ring, one pixel of glass rim.
            $r2 = $R - [Math]::Round($R * 0.2)
            Draw-Ring $g $cx $cy $r2 1 $true (Hex '#7C8899')
            $r3 = $r2 - 1
        } else {
            # 24 px: one silver ring, then the glass rim.
            $r3 = $R - 2
        }
        $p = Circle-Path $cx $cy $r3
        Fill-Solid $g $p $(if ($lv -ge 2) { Hex '#1E2B40' } else { Hex '#2B4466' }); $p.Dispose()
        Draw-LensScene $g $k $cx $cy ($r3 - [Math]::Max(0.8, $ln))
        if ($lv -ge 2) {
            # A thin specular sweep on the outer bezel, top left.
            $pen = New-Object System.Drawing.Pen((C 200 255 255 255), [single]([Math]::Max(1, $R * 0.05)))
            $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
            $ar = $R * 0.9
            $g.DrawArc($pen, [single]($cx - $ar), [single]($cy - $ar), [single](2 * $ar), [single](2 * $ar), 200, 55)
            $pen.Dispose()
        }
    }
    $outer.Dispose(); $inner.Dispose()
}

# The body silhouette (for the drop shadow).
function Draw-CameraSilhouette($g, [double]$s, [double]$ox = 0, [double]$oy = 0) {
    $k = Camera-Geometry $s $ox $oy
    $p = RoundRect-Path $k.bx $k.by ($k.bw * 1.03) ($k.bh * 1.04) $k.br
    Fill-Solid $g $p (C 255 0 0 0); $p.Dispose()
}

# Glossy XP status badge (streaming / paused / error) centred at (cx, cy) with radius r and a white ring.
function Draw-BadgeAt($g, [double]$cx, [double]$cy, [double]$r, [string]$state, [double]$ring) {
    $colors = @{
        streaming = @((Hex '#9BF08A'), (Hex '#3FAA3F'), (Hex '#1D5E1D'))
        paused    = @((Hex '#FFE69A'), (Hex '#E08A00'), (Hex '#8F5200'))
        error     = @((Hex '#FF9D90'), (Hex '#C81E0F'), (Hex '#7A1008'))
    }
    if (-not $colors.ContainsKey($state)) { return }
    $c = $colors[$state]
    $white = New-Object System.Drawing.SolidBrush((Hex '#FFFFFF'))
    # Soft shadow under the badge, then the white outline.
    if ($r -ge 8) {
        $sp = Circle-Path ($cx + $r * 0.06) ($cy + $r * 0.1) ($r + $ring + $r * 0.06)
        Fill-Solid $g $sp (C 70 0 0 0); $sp.Dispose()
    }
    $rr = $r + $ring
    $g.FillEllipse($white, [single]($cx - $rr), [single]($cy - $rr), [single](2 * $rr), [single](2 * $rr))
    $edge = Circle-Path $cx $cy $r
    Fill-Solid $g $edge $c[2]; $edge.Dispose()
    $e = [Math]::Max(0.7, $r * 0.07)
    Fill-Sphere $g $cx $cy ($r - $e) $c[0] $c[1] -0.25 -0.45
    $u = $r
    switch ($state) {
        'streaming' {
            $tri = [System.Drawing.PointF[]]@((PtF ($cx - 0.32 * $u) ($cy - 0.48 * $u)), (PtF ($cx + 0.52 * $u) ($cy)), (PtF ($cx - 0.32 * $u) ($cy + 0.48 * $u)))
            $g.FillPolygon($white, $tri)
        }
        'paused' {
            $w = 0.24 * $u; $h = 0.9 * $u
            $p1 = RoundRect-Path ($cx - 0.38 * $u) ($cy - $h / 2) $w $h ($w * 0.2); $g.FillPath($white, $p1); $p1.Dispose()
            $p2 = RoundRect-Path ($cx + 0.14 * $u) ($cy - $h / 2) $w $h ($w * 0.2); $g.FillPath($white, $p2); $p2.Dispose()
        }
        'error' {
            $pen = New-Object System.Drawing.Pen((Hex '#FFFFFF'), [single]([Math]::Max(1.4, 0.24 * $u)))
            $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
            $d = 0.34 * $u
            $g.DrawLine($pen, [single]($cx - $d), [single]($cy - $d), [single]($cx + $d), [single]($cy + $d))
            $g.DrawLine($pen, [single]($cx + $d), [single]($cy - $d), [single]($cx - $d), [single]($cy + $d))
            $pen.Dispose()
        }
    }
    $white.Dispose()
    if ($r -ge 6) { Draw-Gloss $g $cx $cy ($r - $e) 150 }
}

# Badge in the bottom-right corner of an s x s icon.
function Draw-Badge($g, [double]$s, [string]$state) {
    $ring = if ($s -lt 24) { 1 } else { [Math]::Max(2, $s / 48) }
    $r = if ($s -lt 23) { $s * 0.3 } elseif ($s -lt 46) { $s * 0.26 } else { $s * 0.21 }
    if ($s -lt 46) { $r = [Math]::Round($r * 2) / 2 }
    $pad = if ($s -ge 46) { $r * 0.14 } else { 0 }
    $c = $s - $r - $ring - $pad
    Draw-BadgeAt $g $c $c $r $state $ring
}

# Camera icon for a state: 'ready' (plain), 'disconnected' (desaturated), 'streaming' / 'paused' / 'error'
# (badge). Drawn natively at each size; small sizes use simplified, pixel-snapped geometry.
function Render-Icon([int]$s, [string]$state, [bool]$shadow = $true) {
    $bmp, $g = New-Canvas $s $s
    $lv = Camera-Level $s
    if ($shadow -and $s -ge 24) {
        $sh, $sg = New-Canvas $s $s
        $off = [Math]::Max(1, $s * 0.025)
        Draw-CameraSilhouette $sg $s 0 $off
        $sg.Dispose()
        [LunaFx]::Blur($sh, [Math]::Max(0.6, $s * 0.025))
        $sil = [LunaFx]::Silhouette($sh, (C 255 0 18 60), 0.42)
        $g.DrawImageUnscaled($sil, 0, 0); $sil.Dispose(); $sh.Dispose()
    }
    Draw-Camera $g $s 0 0
    $g.Dispose()
    if ($state -eq 'disconnected') { [LunaFx]::Inactive($bmp) }
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    Set-Quality $g
    Draw-Badge $g $s $state
    $g.Dispose()
    $bmp
}

# =====================================================================================================
#  Sunny hills scene (original, procedural)
# =====================================================================================================

# One cumulus cloud: many soft puffs piled into a dome with a flat base. Two passes give it volume
# without bubble outlines: every puff first lays a cool shadow disc (offset down-right), then every puff
# lays its sunlit white disc (offset up-left) on top, so shading survives only on the outer lower edges.
# Drawn into its own layer, softened, base-shaded, then composited onto the sky.
function Draw-Cloud([System.Drawing.Bitmap]$sky, [double]$cx, [double]$base, [double]$width, [int]$seed, [double]$soft) {
    $W = $sky.Width; $H = $sky.Height
    $rnd = New-Object System.Random $seed
    $layer, $g = New-Canvas $W $H
    $tall = $width * 0.5
    $puffs = New-Object System.Collections.ArrayList
    $n = 22 + [int]($width / 40)
    if ($n -gt 40) { $n = 40 }
    for ($i = 0; $i -lt $n; $i++) {
        $t = $rnd.NextDouble() * 2 - 1
        $t = [Math]::Sign($t) * [Math]::Pow([Math]::Abs($t), 1.25)
        $env = [Math]::Pow([Math]::Max(0, 1 - $t * $t), 0.75)
        $r = $width * (0.06 + 0.08 * $env) * (0.75 + 0.5 * $rnd.NextDouble())
        $x = $cx + $t * $width * 0.38
        $y = $base - $tall * $env * (0.45 + 0.55 * $rnd.NextDouble()) + $r
        if ($y -gt $base - $r * 0.3) { $y = $base - $r * 0.3 }
        [void]$puffs.Add(@($x, $y, $r))
    }
    $g.SetClip((RectF 0 0 $W $base))
    $shadeB = New-Object System.Drawing.SolidBrush((Hex '#BCCADC'))
    foreach ($p in $puffs) {
        $x = $p[0]; $y = $p[1]; $r = $p[2]
        $g.FillEllipse($shadeB, [single]($x - $r + 0.10 * $r), [single]($y - $r + 0.16 * $r), [single](2 * $r), [single](2 * $r))
    }
    $shadeB.Dispose()
    foreach ($p in $puffs) {
        $x = $p[0] - 0.04 * $p[2]; $y = $p[1] - 0.07 * $p[2]; $r = $p[2] * 0.9
        $path = Circle-Path $x $y $r
        $pg = New-Object System.Drawing.Drawing2D.PathGradientBrush($path)
        $pg.CenterPoint = PtF ($x - 0.3 * $r) ($y - 0.45 * $r)
        $pg.CenterColor = (Hex '#FFFFFF')
        $pg.SurroundColors = [System.Drawing.Color[]]@((Hex '#EDF2F8'))
        $g.FillPath($pg, $path); $pg.Dispose(); $path.Dispose()
    }
    $g.ResetClip()
    $g.Dispose()
    [LunaFx]::Blur($layer, $soft)
    # Shaded base: a cool grey-blue towards the flat bottom of the cloud.
    $top = $base - $tall * 0.45
    $shade, $sg = New-Canvas $W $H
    $sb = Linear-Brush (RectF 0 $top $W ($base - $top + 2)) (C 0 0 0 0) (C 0 0 0 0) 90 ([single[]]@(0, 0.5, 1)) @((C 0 150 170 200), (C 40 150 170 200), (C 120 140 162 196))
    $sg.FillRectangle($sb, 0, [single]$top, $W, [single]($base - $top + 2)); $sb.Dispose(); $sg.Dispose()
    [LunaFx]::Atop($layer, $shade, 1.0)
    $shade.Dispose()
    [LunaFx]::Over($sky, $layer, 0.96)
    $layer.Dispose()
}

# A hill as a closed path: a smooth top edge through the given points (Catmull-Rom style curve),
# filled down past the bottom of the canvas.
function Hill-Path([double]$W, [double]$H, [double[]]$xy) {
    $pts = New-Object System.Collections.Generic.List[System.Drawing.PointF]
    for ($i = 0; $i -lt $xy.Count; $i += 2) { $pts.Add((PtF ($xy[$i] * $W) ($xy[$i + 1] * $H))) }
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddCurve($pts.ToArray(), [single]0.5)
    $last = $pts[$pts.Count - 1]; $first = $pts[0]
    $p.AddLine($last, (PtF $last.X ($H + 10)))
    $p.AddLine((PtF $last.X ($H + 10)), (PtF $first.X ($H + 10)))
    $p.CloseFigure()
    $top = New-Object System.Drawing.Drawing2D.GraphicsPath
    $top.AddCurve($pts.ToArray(), [single]0.5)
    @($p, $top)
}

# Paints the hills scene into a new w x h bitmap (32-bit, opaque).
#   -Morning : the paler "early morning" tint used by the Waiting picture.
#   -Clouds  : $false leaves the sky clear.
#   -CloudLayout : optional list of (x, base y, width) fractions replacing the default clouds.
function Paint-Hills([int]$w, [int]$h, [switch]$Morning, [bool]$Clouds = $true, [double]$horizon = 0.58, $CloudLayout = $null) {
    if ($w -lt $h * 1.45) {
        # Square and tall formats (launcher tile, store icon, wizard panel): paint a wider scene and keep
        # its centre, so the hills stay gentle instead of being squeezed into mountains.
        $vw = [int][Math]::Ceiling($h * 1.45)
        $wide = Paint-Hills $vw $h -Morning:$Morning -Clouds $Clouds -horizon $horizon -CloudLayout $CloudLayout
        $crop = $wide.Clone((New-Object System.Drawing.Rectangle ([int](($vw - $w) / 2)), 0, $w, $h), $wide.PixelFormat)
        $wide.Dispose()
        return $crop
    }
    $bmp, $g = New-Canvas $w $h $false
    $hz = $horizon
    # Sky: deep Luna sky at the top, pale at the horizon; a faint sunlit glow from the upper left.
    $skyRect = RectF 0 0 $w ($h * $hz + 2)
    $sb = Linear-Brush $skyRect (Hex '#2E7FE0') (Hex '#9CC8F2') 90 ([single[]]@(0, 0.55, 1)) @((Hex '#2E7FE0'), (Hex '#5FA0EA'), (Hex '#9CC8F2'))
    $g.FillRectangle($sb, 0, 0, $w, $h); $sb.Dispose()
    $gp = Circle-Path ($w * 0.1) (-$h * 0.2) ($w * 0.6)
    $pg = New-Object System.Drawing.Drawing2D.PathGradientBrush($gp)
    $pg.CenterColor = (C 40 170 225 255); $pg.SurroundColors = [System.Drawing.Color[]]@((C 0 170 225 255))
    $g.FillPath($pg, $gp); $pg.Dispose(); $gp.Dispose()
    $g.Dispose()

    $u = [Math]::Min($w, $h * 16 / 9)   # clouds scale with the scene width (16:9 reference)
    if ($Clouds) {
        $soft = [Math]::Max(0.6, $u / 800)
        # (x, base y, width) as fractions; smaller, flatter clouds near the horizon for depth.
        $cloudDefs = @(
            @(0.29, 0.34, 0.26), @(0.51, 0.245, 0.20), @(0.83, 0.21, 0.17), @(0.06, 0.42, 0.15),
            @(0.93, 0.48, 0.19), @(0.65, 0.45, 0.12), @(0.40, 0.52, 0.09), @(0.68, 0.11, 0.08),
            @(0.14, 0.135, 0.07), @(0.54, 0.54, 0.055)
        )
        if ($CloudLayout) { $cloudDefs = $CloudLayout }
        $seed = 11
        foreach ($cl in $cloudDefs) {
            Draw-Cloud $bmp ($cl[0] * $w) ($cl[1] * $h) ($cl[2] * $u) $seed $soft
            $seed += 7
        }
    }

    # Hills, back to front, each in its own layer: gradient, crest light, side light, grass texture.
    $hills, $hg = New-Canvas $w $h
    $hg.Dispose()
    $defs = @(
        # far left ridge: light yellow-green, a little hazy
        @{ pts = @(-0.05, ($hz + 0.07), 0.10, ($hz + 0.02), 0.24, ($hz + 0.015), 0.40, ($hz + 0.07), 0.55, ($hz + 0.16));
           top = '#B4E25A'; bot = '#7CC63A'; crest = '#E6FA9A'; seed = 3 },
        # far right ridge
        @{ pts = @(0.55, ($hz + 0.20), 0.74, ($hz + 0.075), 0.90, ($hz + 0.04), 1.05, ($hz + 0.05));
           top = '#A6DC50'; bot = '#6DBE34'; crest = '#E2F894'; seed = 5 },
        # main hill: crest right of centre (the horizon line)
        @{ pts = @(0.10, ($hz + 0.30), 0.32, ($hz + 0.12), 0.52, ($hz + 0.005), 0.64, ($hz - 0.012), 0.78, ($hz + 0.05), 0.94, ($hz + 0.16), 1.06, ($hz + 0.21));
           top = '#9BD640'; bot = '#3E9A22'; crest = '#D8F57A'; seed = 7 },
        # foreground hill sweeping in from the left
        @{ pts = @(-0.06, ($hz + 0.13), 0.10, ($hz + 0.10), 0.28, ($hz + 0.15), 0.50, ($hz + 0.30), 0.72, ($hz + 0.45));
           top = '#8ED23C'; bot = '#2F8A1F'; crest = '#D2F27A'; seed = 9 },
        # near right swell, bottom corner
        @{ pts = @(0.48, ($hz + 0.47), 0.70, ($hz + 0.33), 0.88, ($hz + 0.30), 1.06, ($hz + 0.32));
           top = '#7CC836'; bot = '#2F8A1F'; crest = '#C8EE70'; seed = 13 }
    )
    $sig = [Math]::Max(0.8, $h / 720)
    foreach ($d in $defs) {
        $path, $topLine = Hill-Path $w $h ([double[]]$d.pts)
        $bounds = $path.GetBounds()
        $layer, $lg = New-Canvas $w $h
        $crestY = $bounds.Y
        $fill = Linear-Brush (RectF 0 $crestY $w ([Math]::Max(4, $h * 0.42))) (Hex $d.top) (Hex $d.bot) 90
        $lg.FillPath($fill, $path); $fill.Dispose()
        $lg.Dispose()

        # Side light from the upper left: lighter on the left slopes, deeper on the right.
        $side, $sg = New-Canvas $w $h
        $sideB = Linear-Brush (RectF $bounds.X $bounds.Y $bounds.Width ([Math]::Max(4, $h * 0.4))) (C 0 0 0 0) (C 0 0 0 0) 15 ([single[]]@(0, 0.45, 1)) @((C 45 255 255 210), (C 0 255 255 255), (C 55 10 60 10))
        $sg.FillRectangle($sideB, 0, 0, $w, $h); $sideB.Dispose(); $sg.Dispose()
        [LunaFx]::Atop($layer, $side, 1.0); $side.Dispose()

        # Crest light: a broad soft band hugging the top edge.
        $cr, $cg = New-Canvas $w $h
        $pen = New-Object System.Drawing.Pen((Hex $d.crest 170), [single]($h * 0.05))
        $cg.DrawPath($pen, $topLine); $pen.Dispose(); $cg.Dispose()
        [LunaFx]::Blur($cr, $h * 0.018)
        [LunaFx]::Atop($layer, $cr, 0.8); $cr.Dispose()
        # A crisp bright rim right on the edge.
        $rim, $rg = New-Canvas $w $h
        $pen = New-Object System.Drawing.Pen((Hex $d.crest 150), [single]([Math]::Max(1, $h * 0.004)))
        $rg.DrawPath($pen, $topLine); $pen.Dispose(); $rg.Dispose()
        [LunaFx]::Blur($rim, $sig * 0.8)
        [LunaFx]::Atop($layer, $rim, 1.0); $rim.Dispose()

        # Grass: fine grain plus long, low-contrast sweeps.
        [LunaFx]::Noise($layer, $d.seed, [single]2.2, [single]5.5, [single]([Math]::Max(6, $w * 0.05)), [single]([Math]::Max(2, $h * 0.012)))

        # The new hill shades what lies just behind its edge (soft valley shadow), then sits on top.
        $sh = [LunaFx]::Silhouette($layer, (C 255 20 60 10), 0.55)
        [LunaFx]::Blur($sh, $h * 0.022)
        $shifted, $shg = New-Canvas $w $h
        $shg.DrawImageUnscaled($sh, 0, [int](-$h * 0.012)); $shg.Dispose(); $sh.Dispose()
        [LunaFx]::Atop($hills, $shifted, 1.0); $shifted.Dispose()
        [LunaFx]::Over($hills, $layer, 1.0)
        $layer.Dispose(); $path.Dispose(); $topLine.Dispose()
    }
    # Hills sit on the sky with an anti-aliased edge (a whisper of blur takes off the GDI+ stair-steps).
    [LunaFx]::Over($bmp, $hills, 1.0)
    $hills.Dispose()

    if ($Morning) { Tint-Morning $bmp $hz }
    $bmp
}

# Early morning: paler, softer, with a faint warm haze low in the sky. Used by the procedural scene and
# by the photo scene, so both "morning" variants match.
function Tint-Morning([System.Drawing.Bitmap]$bmp, [double]$horizon = 0.58) {
    $w = $bmp.Width; $h = $bmp.Height
    [LunaFx]::Desaturate($bmp, 0.16, 0.13)
    $hz2, $zg = New-Canvas $w $h
    $hb = Linear-Brush (RectF 0 ($h * ($horizon - 0.3)) $w ($h * 0.45)) (C 0 0 0 0) (C 0 0 0 0) 90 ([single[]]@(0, 0.6, 1)) @((C 0 255 236 205), (C 70 255 236 205), (C 0 255 236 205))
    $zg.FillRectangle($hb, 0, 0, $w, $h); $hb.Dispose(); $zg.Dispose()
    [LunaFx]::Atop($bmp, $hz2, 1.0); $hz2.Dispose()
}

# =====================================================================================================
#  The hills photograph (design\assets\source\hills-photo.png)
# =====================================================================================================

$script:LunaPhoto = $null

# The source photo, loaded once per session from its bytes (so the file is never left locked).
function Get-HillsPhoto {
    if (-not $script:LunaPhoto) {
        $path = Join-Path (Split-Path $PSScriptRoot -Parent) 'assets\source\hills-photo.png'
        $ms = New-Object System.IO.MemoryStream (, [System.IO.File]::ReadAllBytes($path))
        $script:LunaPhoto = [System.Drawing.Bitmap]::FromStream($ms)
    }
    $script:LunaPhoto
}

# The photo scaled to cover w x h and cropped (anchor 0 = top / left, 1 = bottom / right), optionally
# blurred (sigma given for 720p and scaled with the height) and tinted for the morning variant.
#   The default vertical anchor puts the horizon at about 57 % of the frame, where the XP scene had it.
#   `zoom` above 1 crops in further, which is how a tall panel keeps a gentle horizon instead of a slice.
function Photo-Scene([int]$w, [int]$h, [switch]$Morning, [double]$blur = 0, [double]$anchorY = 0.68, [double]$anchorX = 0.5, [double]$zoom = 1) {
    $src = Get-HillsPhoto
    $scale = [Math]::Max($w / [double]$src.Width, $h / [double]$src.Height) * $zoom
    $sw = [int][Math]::Ceiling($src.Width * $scale); $sh = [int][Math]::Ceiling($src.Height * $scale)
    $bmp, $g = New-Canvas $w $h $false
    $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    $g.CompositingQuality = 'HighQuality'; $g.SmoothingMode = 'AntiAlias'
    $ia = New-Object System.Drawing.Imaging.ImageAttributes
    $ia.SetWrapMode([System.Drawing.Drawing2D.WrapMode]::TileFlipXY)   # no dark fringe along the edges
    $dst = New-Object System.Drawing.Rectangle ([int][Math]::Round(-($sw - $w) * $anchorX)), ([int][Math]::Round(-($sh - $h) * $anchorY)), $sw, $sh
    $g.DrawImage($src, $dst, 0, 0, $src.Width, $src.Height, [System.Drawing.GraphicsUnit]::Pixel, $ia)
    $ia.Dispose(); $g.Dispose()
    if ($blur -gt 0) { [LunaFx]::Blur($bmp, $blur * $h / 720.0) }
    if ($Morning) { Tint-Morning $bmp }
    $bmp
}

# Saves a JPEG at the given quality (GDI+ has no shorthand for this).
function Save-Jpeg([System.Drawing.Bitmap]$bmp, [string]$path, [int]$quality) {
    $codec = [System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object { $_.MimeType -eq 'image/jpeg' }
    $ps = New-Object System.Drawing.Imaging.EncoderParameters 1
    $ps.Param[0] = New-Object System.Drawing.Imaging.EncoderParameter ([System.Drawing.Imaging.Encoder]::Quality), ([int64]$quality)
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    $rgb = To-Rgb24 $bmp
    $rgb.Save($path, $codec, $ps)
    $rgb.Dispose(); $ps.Dispose()
    (Get-Item $path).Length
}

# Saves a JPEG that fits `maxBytes`, dropping the quality a step at a time. Returns "quality/bytes".
function Save-JpegUnder([System.Drawing.Bitmap]$bmp, [string]$path, [int]$maxBytes, [int[]]$qualities = @(88, 85, 82, 78, 74, 70)) {
    foreach ($q in $qualities) {
        $size = Save-Jpeg $bmp $path $q
        if ($size -le $maxBytes) { return "q$q, $([Math]::Round($size / 1KB)) KB" }
    }
    "q$($qualities[-1]), $([Math]::Round((Get-Item $path).Length / 1KB)) KB (over budget)"
}

# =====================================================================================================
#  Glass, progress track, wordmark
# =====================================================================================================

# Frosted glass card on `scene` (modified in place). `rect` = RectangleF. The area behind the card is
# blurred strongly (sigma `blur`), white is laid over at 55 %, with a 1 px white 70 % rim and a soft shadow.
function Draw-FrostedCard([System.Drawing.Bitmap]$scene, $rect, [double]$radius, [double]$blur) {
    $W = $scene.Width; $H = $scene.Height
    $frost = Copy-Bitmap $scene
    [LunaFx]::Blur($frost, $blur)
    $card = RoundRect-Path $rect.X $rect.Y $rect.Width $rect.Height $radius

    # Soft shadow first.
    $sh, $sg = New-Canvas $W $H
    $sp = RoundRect-Path ($rect.X + 2) ($rect.Y + $H * 0.012) $rect.Width $rect.Height $radius
    Fill-Solid $sg $sp (C 255 10 30 70); $sp.Dispose(); $sg.Dispose()
    [LunaFx]::Blur($sh, $H * 0.022)
    [LunaFx]::Over($scene, $sh, 0.30); $sh.Dispose()

    $g = [System.Drawing.Graphics]::FromImage($scene)
    Set-Quality $g
    $tb = New-Object System.Drawing.TextureBrush($frost)
    $g.FillPath($tb, $card); $tb.Dispose()
    Fill-Solid $g $card (C 140 255 255 255)
    # A faint top-to-bottom sheen keeps the glass from looking like flat paint.
    $gl = Linear-Brush $rect (C 60 255 255 255) (C 0 255 255 255) 90 ([single[]]@(0, 0.45, 1)) @((C 70 255 255 255), (C 0 255 255 255), (C 18 200 220 255))
    $g.FillPath($gl, $card); $gl.Dispose()
    $pen = New-Object System.Drawing.Pen((C 178 255 255 255), [single]1)
    $rim = RoundRect-Path ($rect.X + 0.5) ($rect.Y + 0.5) ($rect.Width - 1) ($rect.Height - 1) ($radius - 0.5)
    $g.DrawPath($pen, $rim); $pen.Dispose(); $rim.Dispose()
    $g.Dispose(); $card.Dispose(); $frost.Dispose()
}

# The empty XP progress track: rounded white track with a #ACA899 border and a faint inner shadow.
function Draw-ProgressTrack($g, [double]$x, [double]$y, [double]$w, [double]$h) {
    $r = [Math]::Max(2, $h * 0.12)
    $o = RoundRect-Path $x $y $w $h $r
    Fill-Solid $g $o (Hex '#ACA899'); $o.Dispose()
    $i = RoundRect-Path ($x + 1) ($y + 1) ($w - 2) ($h - 2) ($r - 1)
    Fill-Solid $g $i (Hex '#FFFFFF')
    $st = $g.Save(); $g.SetClip($i)
    $sb = Linear-Brush (RectF ($x + 1) ($y + 1) ($w - 2) ($h * 0.35)) (C 40 120 116 100) (C 0 120 116 100) 90
    $g.FillRectangle($sb, [single]($x + 1), [single]($y + 1), [single]($w - 2), [single]($h * 0.35)); $sb.Dispose()
    $g.Restore($st); $i.Dispose()
}

# Text with a soft blurred shadow (wordmarks over imagery).
function Draw-SoftText([System.Drawing.Bitmap]$bmp, [string]$text, $font, $color, [double]$x, [double]$y, [double]$blur = 3, [int]$shadowAlpha = 150) {
    $sh, $sg = New-Canvas $bmp.Width $bmp.Height
    $sg.TextRenderingHint = 'AntiAlias'
    $b = New-Object System.Drawing.SolidBrush((C $shadowAlpha 10 30 60))
    $sg.DrawString($text, $font, $b, [single]($x + 1), [single]($y + 2)); $b.Dispose(); $sg.Dispose()
    [LunaFx]::Blur($sh, $blur)
    [LunaFx]::Over($bmp, $sh, 1.0); $sh.Dispose()
    $g = [System.Drawing.Graphics]::FromImage($bmp); Set-Quality $g
    $g.TextRenderingHint = 'AntiAlias'
    $b = New-Object System.Drawing.SolidBrush($color)
    $g.DrawString($text, $font, $b, [single]$x, [single]$y); $b.Dispose(); $g.Dispose()
}

# Typographic string format that keeps spaces (GDI+ trims them by default when measuring).
function Typo-Format() {
    $f = New-Object System.Drawing.StringFormat ([System.Drawing.StringFormat]::GenericTypographic)
    $f.FormatFlags = $f.FormatFlags -bor [System.Drawing.StringFormatFlags]::MeasureTrailingSpaces
    , $f
}

function Measure-Spaced($g, [string]$text, $font, [double]$spacing) {
    $fmt = Typo-Format
    $total = 0.0
    foreach ($ch in $text.ToCharArray()) {
        $total += $g.MeasureString([string]$ch, $font, [System.Drawing.PointF]::Empty, $fmt).Width + $spacing
    }
    $fmt.Dispose()
    if ($text.Length -gt 0) { $total - $spacing } else { 0.0 }
}

function Draw-SpacedOn($g, [string]$text, $font, $brush, [double]$x, [double]$y, [double]$spacing) {
    $fmt = Typo-Format
    foreach ($ch in $text.ToCharArray()) {
        $s = [string]$ch
        $g.DrawString($s, $font, $brush, [single]$x, [single]$y, $fmt)
        $x += $g.MeasureString($s, $font, [System.Drawing.PointF]::Empty, $fmt).Width + $spacing
    }
    $fmt.Dispose()
}

# Letter-spaced text centred on `centerX`, with the soft shadow that keeps light type readable over
# imagery. Returns the width it drew.
function Draw-SpacedText([System.Drawing.Bitmap]$bmp, [string]$text, $font, $color, [double]$centerX, [double]$y, [double]$spacing, [double]$blur = 3, [int]$shadowAlpha = 150) {
    $mg = [System.Drawing.Graphics]::FromImage($bmp); $mg.TextRenderingHint = 'AntiAlias'
    $w = Measure-Spaced $mg $text $font $spacing
    $mg.Dispose()
    $x = $centerX - $w / 2
    $sh, $sg = New-Canvas $bmp.Width $bmp.Height
    $sg.TextRenderingHint = 'AntiAlias'
    $b = New-Object System.Drawing.SolidBrush((C $shadowAlpha 8 26 56))
    Draw-SpacedOn $sg $text $font $b ($x + 1) ($y + 2) $spacing
    $b.Dispose(); $sg.Dispose()
    [LunaFx]::Blur($sh, $blur)
    [LunaFx]::Over($bmp, $sh, 1.0); $sh.Dispose()
    $g = [System.Drawing.Graphics]::FromImage($bmp); Set-Quality $g; $g.TextRenderingHint = 'AntiAlias'
    $b = New-Object System.Drawing.SolidBrush($color)
    Draw-SpacedOn $g $text $font $b $x $y $spacing
    $b.Dispose(); $g.Dispose()
    $w
}

# The "video off" glyph of the paused picture: a camcorder (rounded body plus a lens horn) with a slash
# across it, the slash separated from the body by a clean gap. Drawn into its own layer so the gap can be
# cut out of the shape rather than painted over it. `w` is the full width, including the slash overhang.
function Draw-CamcorderOff([System.Drawing.Bitmap]$dst, [double]$cx, [double]$cy, [double]$w, $color, [double]$opacity = 1.0) {
    $layer, $g = New-Canvas $dst.Width $dst.Height
    $bw = $w * 0.60; $bh = $w * 0.42; $r = $w * 0.075
    $bx = $cx - $w * 0.45; $by = $cy - $bh / 2
    $body = RoundRect-Path $bx $by $bw $bh $r
    Fill-Solid $g $body $color; $body.Dispose()
    # Lens horn: a triangle whose point meets the body, flat edge outwards.
    $gap = $w * 0.025
    $hx = $bx + $bw + $gap; $hw = $w * 0.19; $hh = $w * 0.30
    $horn = New-Object System.Drawing.Drawing2D.GraphicsPath
    $horn.AddPolygon([System.Drawing.PointF[]]@((PtF $hx $cy), (PtF ($hx + $hw) ($cy - $hh / 2)), (PtF ($hx + $hw) ($cy + $hh / 2))))
    Fill-Solid $g $horn $color; $horn.Dispose()
    $g.Dispose()

    # Cut the gap, then lay the slash into it.
    $d = $w * 0.42; $k = 0.72          # the slash runs a little flatter than 45 degrees
    $p1 = PtF ($cx - $d) ($cy - $d * $k); $p2 = PtF ($cx + $d) ($cy + $d * $k)
    $stroke = [Math]::Max(1.5, $w * 0.072)
    $cut, $cg = New-Canvas $dst.Width $dst.Height
    $pen = New-Object System.Drawing.Pen($color, [single]($stroke + 2 * $w * 0.028))
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $cg.DrawLine($pen, $p1, $p2); $pen.Dispose(); $cg.Dispose()
    [LunaFx]::CutAlpha($layer, $cut); $cut.Dispose()
    $g = [System.Drawing.Graphics]::FromImage($layer); Set-Quality $g
    $pen = New-Object System.Drawing.Pen($color, [single]$stroke)
    $pen.StartCap = 'Round'; $pen.EndCap = 'Round'
    $g.DrawLine($pen, $p1, $p2); $pen.Dispose(); $g.Dispose()

    [LunaFx]::Over($dst, $layer, [single]$opacity)
    $layer.Dispose()
}

# =====================================================================================================
#  The mark as a silhouette: one geometry for every outline asset
# =====================================================================================================

# The hill inside the lens (same curve as Draw-LensScene's front hill), as a path to clip to the glass.
function Mark-HillPath([double]$cx, [double]$cy, [double]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $p.AddBezier((PtF ($cx - 1.2 * $r) ($cy + 0.28 * $r)), (PtF ($cx - 0.6 * $r) ($cy - 0.42 * $r)), (PtF ($cx - 0.1 * $r) ($cy - 0.38 * $r)), (PtF ($cx + 0.35 * $r) ($cy + 0.02 * $r)))
    $p.AddBezier((PtF ($cx + 0.35 * $r) ($cy + 0.02 * $r)), (PtF ($cx + 0.62 * $r) ($cy + 0.22 * $r)), (PtF ($cx + 0.85 * $r) ($cy + 0.18 * $r)), (PtF ($cx + 1.2 * $r) ($cy + 0.06 * $r)))
    $p.AddLine((PtF ($cx + 1.2 * $r) ($cy + 0.06 * $r)), (PtF ($cx + 1.2 * $r) ($cy + 1.3 * $r)))
    $p.AddLine((PtF ($cx + 1.2 * $r) ($cy + 1.3 * $r)), (PtF ($cx - 1.2 * $r) ($cy + 1.3 * $r)))
    $p.CloseFigure()
    , $p
}

# The silhouette of the "Snap" mark in an s x s box at (ox, oy), as named GraphicsPaths. Every outline
# asset (the themed icon, the notification icon, the preview sheet) is built from these, so the body
# proportions, the corner radius and the lens position can never drift from the colour logo: they all
# come from Camera-Geometry.
#   body    body outline with the lens opening cut out (even-odd)
#   bezel   the ring around the glass (even-odd)
#   glass   the circle the hill is clipped to
#   hill    the hill inside the glass
#   shutter the shutter button on top
#   light   the "on" light
function Mark-Paths([double]$s, [double]$ox = 0, [double]$oy = 0) {
    $k = Camera-Geometry $s $ox $oy
    $ln = [Math]::Max(1.0, $s * 0.028)
    $m = @{}
    $body = RoundRect-Path $k.bx $k.by $k.bw $k.bh $k.br
    $body.AddPath((Circle-Path $k.cx $k.cy $k.R), $false)
    $body.FillMode = 'Alternate'
    $m.body = $body
    $bezel = Circle-Path $k.cx $k.cy ($k.R * 0.82)
    $bezel.AddPath((Circle-Path $k.cx $k.cy ($k.R * 0.63)), $false)
    $bezel.FillMode = 'Alternate'
    $m.bezel = $bezel
    $m.glass = Circle-Path $k.cx $k.cy ($k.R * 0.52)
    $m.hill = Mark-HillPath $k.cx $k.cy ($k.R * 0.52)
    $sw = $k.bw * 0.22; $sh = $s * 0.1
    $m.shutter = RoundRect-Path ($k.bx + $k.bw * 0.66) ($k.by - $s * 0.062) $sw $sh ([Math]::Max(1, $sh * 0.35))
    $m.light = Circle-Path ($k.bx + $k.bw * 0.86) ($k.by + $k.bh * 0.135) ([Math]::Max(1, $k.bh * 0.075))
    $m.geometry = $k
    $m
}

# The mark's silhouette fitted into a square viewport: 'radius' keeps every pixel inside a circle of that
# radius (the Android safe zone), 'width' makes the shape that many units wide. The paths come back
# already transformed, ready to fill with GDI+ or to write out as android:pathData.
function Mark-Silhouette([double]$viewport, [string]$mode, [double]$value) {
    $m = Mark-Paths 100 0 0
    $r = $m.body.GetBounds()
    foreach ($key in 'shutter', 'light') {
        $b = $m[$key].GetBounds()
        $r = [System.Drawing.RectangleF]::Union($r, $b)
    }
    $cx = $r.X + $r.Width / 2; $cy = $r.Y + $r.Height / 2
    $f = if ($mode -eq 'width') { $value / $r.Width }
         else { $value / [Math]::Sqrt([Math]::Pow($r.Width / 2, 2) + [Math]::Pow($r.Height / 2, 2)) }
    $mat = New-Object System.Drawing.Drawing2D.Matrix
    $mat.Translate([single]($viewport / 2), [single]($viewport / 2))
    $mat.Scale([single]$f, [single]$f)
    $mat.Translate([single](-$cx), [single](-$cy))
    foreach ($key in 'shutter', 'body', 'bezel', 'glass', 'hill', 'light') { $m[$key].Transform($mat) }
    $mat.Dispose()
    $m
}

# Fills a Mark-Silhouette into a Graphics in one colour (the themed icon, the notification icon and the
# audit sheet all draw the same paths).
function Draw-MarkSilhouette($g, $m, $color, [bool]$withLight = $true) {
    $b = New-Object System.Drawing.SolidBrush($color)
    $g.FillPath($b, $m.shutter)
    $g.FillPath($b, $m.body)
    $g.FillPath($b, $m.bezel)
    $st = $g.Save(); $g.SetClip($m.glass)
    $g.FillPath($b, $m.hill)
    $g.Restore($st)
    if ($withLight) { $g.FillPath($b, $m.light) }
    $b.Dispose()
}

function Num([double]$v) { $v.ToString('0.###', [System.Globalization.CultureInfo]::InvariantCulture) }

# A GraphicsPath as SVG / android:pathData (M, L, C and z only; GDI+ has already turned arcs into
# beziers), optionally scaled and shifted into a vector drawable's viewport.
function Path-To-PathData([System.Drawing.Drawing2D.GraphicsPath]$path, [double]$scale = 1, [double]$dx = 0, [double]$dy = 0) {
    $pts = $path.PathPoints; $types = $path.PathTypes
    $sb = New-Object System.Text.StringBuilder
    $P = { param($i) "$(Num ($pts[$i].X * $scale + $dx)),$(Num ($pts[$i].Y * $scale + $dy))" }
    $i = 0
    while ($i -lt $pts.Length) {
        $kind = $types[$i] -band 7
        if ($kind -eq 3) {
            [void]$sb.Append("C$(& $P $i) $(& $P ($i + 1)) $(& $P ($i + 2))")
            $i += 3
        } else {
            [void]$sb.Append($(if ($kind -eq 0) { 'M' } else { 'L' }))
            [void]$sb.Append((& $P $i))
            $i++
        }
        if (($types[$i - 1] -band 128) -ne 0) { [void]$sb.Append('z') }
    }
    $sb.ToString()
}
