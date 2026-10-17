# Draws the Tandem icon (a tablet with a blue screen next to a small monitor
# base) and writes a multi-resolution .ico with PNG-compressed images.
param([string]$Out = (Join-Path $PSScriptRoot "..\res\Tandem.ico"))
Add-Type -AssemblyName System.Drawing

function Draw([int]$n) {
    $bmp = New-Object Drawing.Bitmap $n, $n, ([Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([Drawing.Color]::Transparent)

    function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
        $p = New-Object Drawing.Drawing2D.GraphicsPath
        $d = 2 * $r
        $p.AddArc($x, $y, $d, $d, 180, 90)
        $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
        $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
        $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
        $p.CloseFigure()
        return $p
    }
    $s = $n / 256.0
    # Tablet body (landscape), dark frame.
    $body = RoundRect (16 * $s) (40 * $s) (224 * $s) (164 * $s) (22 * $s)
    $g.FillPath((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 36, 38, 46))), $body)
    # Screen with a blue gradient.
    $screenRect = New-Object Drawing.RectangleF (30 * $s), (54 * $s), (196 * $s), (136 * $s)
    $screen = RoundRect $screenRect.X $screenRect.Y $screenRect.Width $screenRect.Height (10 * $s)
    $grad = New-Object Drawing.Drawing2D.LinearGradientBrush $screenRect, ([Drawing.Color]::FromArgb(255, 72, 170, 255)), ([Drawing.Color]::FromArgb(255, 40, 100, 230)), 45.0
    $g.FillPath($grad, $screen)
    # A "window" on the screen to suggest an extended desktop.
    if ($n -ge 32) {
        $win = RoundRect (62 * $s) (82 * $s) (112 * $s) (72 * $s) (6 * $s)
        $g.FillPath((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(230, 255, 255, 255))), $win)
        $bar = New-Object Drawing.RectangleF (62 * $s), (82 * $s), (112 * $s), (14 * $s)
        $g.FillRectangle((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 255, 196, 64))), $bar)
    }
    # USB cable plug under the tablet.
    $g.FillRectangle((New-Object Drawing.SolidBrush ([Drawing.Color]::FromArgb(255, 36, 38, 46))), (112 * $s), (204 * $s), (32 * $s), (30 * $s))
    $g.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = foreach ($n in $sizes) {
    $bmp = Draw $n
    $ms = New-Object IO.MemoryStream
    $bmp.Save($ms, [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    , $ms.ToArray()
}

New-Item -ItemType Directory -Force (Split-Path $Out) | Out-Null
$fs = [IO.File]::Create($Out)
$w = New-Object IO.BinaryWriter $fs
$w.Write([UInt16]0); $w.Write([UInt16]1); $w.Write([UInt16]$sizes.Count)
$offset = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
    $n = $sizes[$i]
    $w.Write([byte]($(if ($n -ge 256) { 0 } else { $n })))
    $w.Write([byte]($(if ($n -ge 256) { 0 } else { $n })))
    $w.Write([byte]0); $w.Write([byte]0)
    $w.Write([UInt16]1); $w.Write([UInt16]32)
    $w.Write([UInt32]$pngs[$i].Length); $w.Write([UInt32]$offset)
    $offset += $pngs[$i].Length
}
foreach ($p in $pngs) { $w.Write($p) }
$w.Close()
"Wrote $Out ($((Get-Item $Out).Length) bytes)"
