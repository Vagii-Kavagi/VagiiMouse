# Store listing screenshots: each page in Store mode, centred on a 1920x1080 dark backdrop.
# Output: bin\store-screenshots\*.png
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$out = Join-Path $root 'bin\store-screenshots'
New-Item -ItemType Directory -Force $out | Out-Null
$env:VAGII_FAKE_STORE = '1'
Add-Type -AssemblyName System.Drawing
$pages = @(@{ n = 0; f = '1-mouse' }, @{ n = 1; f = '2-trackpad' }, @{ n = 2; f = '3-settings' })
foreach ($p in $pages) {
  $raw = Join-Path $out "$($p.f)-raw.png"
  & (Join-Path $root 'tools\shot.ps1') -Test -Extra "--page=$($p.n)" -Out $raw | Out-Null
  $win = [System.Drawing.Image]::FromFile($raw)
  $bmp = New-Object System.Drawing.Bitmap 1920, 1080
  $g = [System.Drawing.Graphics]::FromImage($bmp)
  $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'
  $bg = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.Point]::new(0, 0)), ([System.Drawing.Point]::new(1920, 1080)), ([System.Drawing.Color]::FromArgb(17, 24, 52)), ([System.Drawing.Color]::FromArgb(8, 9, 14))
  $g.FillRectangle($bg, 0, 0, 1920, 1080)
  $x = [int]((1920 - $win.Width) / 2); $y = [int]((1080 - $win.Height) / 2)
  $g.DrawImage($win, $x, $y, $win.Width, $win.Height)
  $g.Dispose(); $win.Dispose()
  $bmp.Save((Join-Path $out "$($p.f).png"), [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
  Remove-Item $raw
  "saved $($p.f).png"
}
