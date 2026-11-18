# Draws res\app.ico (mouse glyph, 16..256 px, PNG-compressed entries).
Add-Type -AssemblyName System.Drawing
$sizes = 16, 20, 24, 32, 48, 64, 256
$pngs = foreach ($s in $sizes) {
  $b = New-Object System.Drawing.Bitmap $s, $s
  $g = [System.Drawing.Graphics]::FromImage($b)
  $g.SmoothingMode = 'AntiAlias'
  $l = $s * 0.24; $t = $s * 0.06; $w = $s * 0.52; $h = $s * 0.88; $r = $s * 0.26 * 2
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $p.AddArc($l, $t, $r, $r, 180, 90); $p.AddArc($l + $w - $r, $t, $r, $r, 270, 90)
  $p.AddArc($l + $w - $r, $t + $h - $r, $r, $r, 0, 90); $p.AddArc($l, $t + $h - $r, $r, $r, 90, 90); $p.CloseFigure()
  $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, $t)), ([System.Drawing.PointF]::new(0, $t + $h)), ([System.Drawing.Color]::FromArgb(37, 99, 235)), ([System.Drawing.Color]::FromArgb(97, 59, 235))
  $g.FillPath($br, $p)
  $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([Math]::Max(1, $s * 0.045))
  $g.DrawLine($pen, $s * 0.5, $t, $s * 0.5, $s * 0.42); $g.DrawLine($pen, $l, $s * 0.42, $l + $w, $s * 0.42)
  $g.Dispose()
  $ms = New-Object System.IO.MemoryStream; $b.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
  ,$ms.ToArray()
}
$out = New-Object System.IO.MemoryStream; $bw = New-Object System.IO.BinaryWriter $out
$bw.Write([uint16]0); $bw.Write([uint16]1); $bw.Write([uint16]$sizes.Count)
$off = 6 + 16 * $sizes.Count
for ($i = 0; $i -lt $sizes.Count; $i++) {
  $s = $sizes[$i]; $d = if ($s -ge 256) { 0 } else { $s }
  $bw.Write([byte]$d); $bw.Write([byte]$d); $bw.Write([byte]0); $bw.Write([byte]0)
  $bw.Write([uint16]1); $bw.Write([uint16]32); $bw.Write([uint32]$pngs[$i].Length); $bw.Write([uint32]$off); $off += $pngs[$i].Length
}
foreach ($p in $pngs) { $bw.Write($p) }
[IO.File]::WriteAllBytes((Join-Path $PSScriptRoot '..\res\app.ico'), $out.ToArray())
