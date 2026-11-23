# Иконка приложения: d20 на тёмной панели в палитре интерфейса.
# Рисует assets/icon.png (256x256), assets/icon.ico (16..256) и встроенный
# массив src/app/icon_png.h для SDL_SetWindowIcon.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_icon.ps1
#
# Перерисовывать нужно только при изменении рисунка: готовые файлы лежат в
# репозитории, сборка их не создаёт.

[CmdletBinding()]
param()

Add-Type -AssemblyName System.Drawing

$Root = Split-Path -Parent $PSScriptRoot
$PngPath = Join-Path $Root 'assets/icon.png'
$IcoPath = Join-Path $Root 'assets/icon.ico'
$HeaderPath = Join-Path $Root 'src/app/icon_png.h'

# Палитра совпадает с menu_renderer: тёмная панель, синие акценты.
$BackgroundTop = [System.Drawing.Color]::FromArgb(0x26, 0x26, 0x34)
$BackgroundBottom = [System.Drawing.Color]::FromArgb(0x10, 0x10, 0x18)
$BorderColor = [System.Drawing.Color]::FromArgb(0x3a, 0x3a, 0x48)
$EdgeColor = [System.Drawing.Color]::FromArgb(0xee, 0xf4, 0xff)
$BaseRed = 0x6c
$BaseGreen = 0xa8
$BaseBlue = 0xff

# Геометрия d20 (доли радиуса, y вниз): шестиугольник-силуэт, передняя грань
# треугольником вверх и шесть спиц от её вершин к вершинам силуэта.
$Hexagon = @(
  @(1.0, 0.0), @(0.5, -0.866), @(-0.5, -0.866),
  @(-1.0, 0.0), @(-0.5, 0.866), @(0.5, 0.866)
)
$Apex = @(0.0, -0.52)
$BaseLeft = @(-0.50, 0.30)
$BaseRight = @(0.50, 0.30)

function New-Point([double]$cx, [double]$cy, [double]$radius, [array]$p) {
  return [System.Drawing.PointF]::new(
    [single]($cx + $p[0] * $radius),
    [single]($cy + $p[1] * $radius))
}

function New-FaceColor([double]$factor) {
  $r = [int][math]::Min(255, [math]::Round($BaseRed * $factor))
  $g = [int][math]::Min(255, [math]::Round($BaseGreen * $factor))
  $b = [int][math]::Min(255, [math]::Round($BaseBlue * $factor))
  return [System.Drawing.Color]::FromArgb($r, $g, $b)
}

function New-RoundedPath([single]$x, [single]$y, [single]$w, [single]$h,
                         [single]$radius) {
  $path = [System.Drawing.Drawing2D.GraphicsPath]::new()
  $d = $radius * 2
  $path.AddArc($x, $y, $d, $d, 180, 90)
  $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
  $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
  $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
  $path.CloseFigure()
  return $path
}

function New-IconBitmap([int]$size) {
  $bmp = [System.Drawing.Bitmap]::new($size, $size,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
  $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
  $g.CompositingQuality =
    [System.Drawing.Drawing2D.CompositingQuality]::HighQuality
  $g.Clear([System.Drawing.Color]::Transparent)

  $inset = [single]($size * 0.035)
  $square = [single]($size - 2 * $inset)
  $corner = [single]($size * 0.20)
  $panel = New-RoundedPath $inset $inset $square $square $corner

  $rect = [System.Drawing.RectangleF]::new($inset, $inset, $square, $square)
  $background = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
    $rect, $BackgroundTop, $BackgroundBottom, 90.0)
  $g.FillPath($background, $panel)
  $background.Dispose()

  $cx = $size / 2.0
  $cy = $size / 2.0
  $radius = $size * 0.34

  $outline = @()
  foreach ($p in $Hexagon) {
    $outline += New-Point $cx $cy $radius $p
  }

  # Мягкая тень: тот же силуэт чуть крупнее и ниже, чтобы д20 не сливался
  # с фоном.
  if ($size -ge 32) {
    $shadow = @()
    foreach ($p in $Hexagon) {
      $shadow += New-Point $cx ($cy + $size * 0.03) ($radius * 1.07) $p
    }
    $shadowBrush = [System.Drawing.SolidBrush]::new(
      [System.Drawing.Color]::FromArgb(0x50, 0x00, 0x00, 0x00))
    $g.FillPolygon($shadowBrush, $shadow)
    $shadowBrush.Dispose()
  }

  $apex = New-Point $cx $cy $radius $Apex
  $left = New-Point $cx $cy $radius $BaseLeft
  $right = New-Point $cx $cy $radius $BaseRight
  $s0 = $outline[0]
  $s60 = $outline[1]
  $s120 = $outline[2]
  $s180 = $outline[3]
  $s240 = $outline[4]
  $s300 = $outline[5]

  $faces = @(
    @{ Points = @($apex, $s60, $s120); Shade = 1.16 },   # верхняя грань
    @{ Points = @($apex, $s120, $s180, $left); Shade = 1.02 },
    @{ Points = @($left, $s180, $s240); Shade = 0.84 },
    @{ Points = @($left, $s240, $s300, $right); Shade = 0.74 },
    @{ Points = @($right, $s300, $s0); Shade = 0.62 },
    @{ Points = @($apex, $s60, $s0, $right); Shade = 0.92 }
  )
  foreach ($face in $faces) {
    $brush = [System.Drawing.SolidBrush]::new((New-FaceColor $face.Shade))
    $g.FillPolygon($brush, $face.Points)
    $brush.Dispose()
  }

  # Передняя грань — самая светлая, с лёгким градиентом сверху вниз.
  $front = @($apex, $left, $right)
  $frontBounds = [System.Drawing.RectangleF]::new(
    [single]$left.X, [single]$apex.Y,
    [single]($right.X - $left.X), [single]($right.Y - $apex.Y))
  $frontBrush = [System.Drawing.Drawing2D.LinearGradientBrush]::new(
    $frontBounds, (New-FaceColor 1.12), (New-FaceColor 0.88), 90.0)
  $g.FillPolygon($frontBrush, $front)
  $frontBrush.Dispose()

  $edgeWidth = [single][math]::Max(1.0, $size * 0.018)
  $edge = [System.Drawing.Pen]::new($EdgeColor, $edgeWidth)
  $edge.LineJoin = [System.Drawing.Drawing2D.LineJoin]::Round

  $g.DrawPolygon($edge, $outline)
  $g.DrawPolygon($edge, $front)
  # Спицы на 16-24 px сливаются в кашу: там достаточно силуэта и грани.
  if ($size -ge 32) {
    $g.DrawLine($edge, $apex, $s60)
    $g.DrawLine($edge, $apex, $s120)
    $g.DrawLine($edge, $left, $s180)
    $g.DrawLine($edge, $left, $s240)
    $g.DrawLine($edge, $right, $s300)
    $g.DrawLine($edge, $right, $s0)
  }
  $edge.Dispose()

  if ($size -ge 48) {
    $borderPen = [System.Drawing.Pen]::new($BorderColor,
      [single][math]::Max(1.0, $size * 0.008))
    $g.DrawPath($borderPen, $panel)
    $borderPen.Dispose()
  }

  $g.Dispose()
  $panel.Dispose()
  return $bmp
}

function Get-PngBytes([System.Drawing.Bitmap]$bitmap) {
  $stream = [System.IO.MemoryStream]::new()
  $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Png)
  $bytes = $stream.ToArray()
  $stream.Dispose()
  return , $bytes
}

# DIB-запись ICO: BITMAPINFOHEADER, пиксели BGRA снизу вверх и пустая AND-маска.
# Порядок каналов в DIB — BGRA, а LockBits отдаёт буфер ровно в нём, поэтому
# строка копируется как есть: перестановка R и B превратила бы синий кубик в
# оранжевый.
function Get-DibBytes([System.Drawing.Bitmap]$bitmap) {
  $w = $bitmap.Width
  $h = $bitmap.Height
  $rect = [System.Drawing.Rectangle]::new(0, 0, $w, $h)
  $locked = $bitmap.LockBits($rect,
    [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
    [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
  $stride = $locked.Stride
  $raw = New-Object byte[] ($stride * $h)
  [System.Runtime.InteropServices.Marshal]::Copy($locked.Scan0, $raw, 0,
    $raw.Length)
  $bitmap.UnlockBits($locked)

  $stream = [System.IO.MemoryStream]::new()
  $writer = [System.IO.BinaryWriter]::new($stream)
  $writer.Write([uint32]40)
  $writer.Write([int32]$w)
  $writer.Write([int32]($h * 2))
  $writer.Write([uint16]1)
  $writer.Write([uint16]32)
  $writer.Write([uint32]0)
  $writer.Write([uint32]($w * $h * 4))
  $writer.Write([int32]0)
  $writer.Write([int32]0)
  $writer.Write([uint32]0)
  $writer.Write([uint32]0)
  for ($y = $h - 1; $y -ge 0; $y--) {
    $row = $y * $stride
    for ($x = 0; $x -lt $w; $x++) {
      $i = $row + $x * 4
      $writer.Write($raw[$i])
      $writer.Write($raw[$i + 1])
      $writer.Write($raw[$i + 2])
      $writer.Write($raw[$i + 3])
    }
  }
  $maskStride = [int]([math]::Floor(($w + 31) / 32) * 4)
  $writer.Write((New-Object byte[] ($maskStride * $h)))
  $writer.Flush()
  $bytes = $stream.ToArray()
  $writer.Dispose()
  $stream.Dispose()
  return , $bytes
}

$sizes = @(16, 20, 24, 32, 40, 48, 64, 128, 256)
$entries = @()
foreach ($size in $sizes) {
  $bitmap = New-IconBitmap $size
  if ($size -ge 128) {
    $data = Get-PngBytes $bitmap
  } else {
    $data = Get-DibBytes $bitmap
  }
  $entries += @{ Size = $size; Data = $data }
  if ($size -eq 256) {
    [System.IO.File]::WriteAllBytes($PngPath, $data)
  }
  $bitmap.Dispose()
}

$headerSize = 6 + 16 * $entries.Count
$offset = $headerSize
$stream = [System.IO.MemoryStream]::new()
$writer = [System.IO.BinaryWriter]::new($stream)
$writer.Write([uint16]0)
$writer.Write([uint16]1)
$writer.Write([uint16]$entries.Count)
foreach ($entry in $entries) {
  $dim = if ($entry.Size -ge 256) { 0 } else { $entry.Size }
  $writer.Write([byte]$dim)
  $writer.Write([byte]$dim)
  $writer.Write([byte]0)
  $writer.Write([byte]0)
  $writer.Write([uint16]1)
  $writer.Write([uint16]32)
  $writer.Write([uint32]$entry.Data.Length)
  $writer.Write([uint32]$offset)
  $offset += $entry.Data.Length
}
foreach ($entry in $entries) {
  $writer.Write($entry.Data)
}
$writer.Flush()
[System.IO.File]::WriteAllBytes($IcoPath, $stream.ToArray())
$writer.Dispose()
$stream.Dispose()

# Встроенная копия 256x256 для SDL_SetWindowIcon: exe остаётся автономным и
# не ищет файл рядом с собой.
$png = [System.IO.File]::ReadAllBytes($PngPath)
$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('#pragma once')
$lines.Add('')
$lines.Add('#include <array>')
$lines.Add('#include <cstddef>')
$lines.Add('')
$lines.Add('namespace dnd {')
$lines.Add('')
$lines.Add('// Иконка окна: PNG 256x256 из assets/icon.png.')
$lines.Add('// Файл создан tools/make_icon.ps1 — правьте рисунок там, не здесь.')
$lines.Add("inline constexpr std::array<unsigned char, $($png.Length)> kIconPng = {")
for ($i = 0; $i -lt $png.Length; $i += 12) {
  $chunk = @()
  $last = [math]::Min($i + 12, $png.Length)
  for ($j = $i; $j -lt $last; $j++) {
    $chunk += ('0x{0:x2}' -f $png[$j])
  }
  $lines.Add('    ' + ($chunk -join ', ') + ',')
}
$lines.Add('};')
$lines.Add('')
$lines.Add('}  // namespace dnd')
$lines.Add('')
[System.IO.File]::WriteAllLines($HeaderPath, $lines)

Write-Host "==> $PngPath"
Write-Host "==> $IcoPath"
Write-Host "==> $HeaderPath"
