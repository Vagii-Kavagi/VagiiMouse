# Builds the Microsoft Store package (MSIX) of Vagii Mouse.
#
#   .\store\build-msix.ps1                 local test package (registered by
#                                          tools\test-msix.ps1; needs Developer Mode)
#   .\store\build-msix.ps1 -Store -Name "<Package/Identity/Name>" -Publisher "<Package/Identity/Publisher>" -PublisherDisplayName "<your Store name>"
#                                          upload package for Partner Center (values from
#                                          Product management > Product identity)
param(
  [switch]$Store,
  [string]$Name = "VagiiMouse.Dev",
  # dev identity for Developer Mode registration (tools\test-msix.ps1)
  [string]$Publisher = "CN=VagiiDev",
  [string]$PublisherDisplayName = "Vagii",
  [switch]$NoBuild
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if ($Store -and ($Name -eq "VagiiMouse.Dev" -or $Publisher -eq "CN=VagiiDev")) {
  throw "-Store needs the Name and Publisher from Partner Center (Product identity page)."
}
if (-not $NoBuild) { & (Join-Path $root 'build.ps1') }

$vh = Get-Content (Join-Path $root 'src\version.h') -Raw
$ver = "{0}.{1}.{2}.0" -f ([regex]::Match($vh, 'APP_VER_MAJOR (\d+)').Groups[1].Value),
  ([regex]::Match($vh, 'APP_VER_MINOR (\d+)').Groups[1].Value), ([regex]::Match($vh, 'APP_VER_PATCH (\d+)').Groups[1].Value)

$layout = Join-Path $root 'obj\msix'
if (Test-Path $layout) { Remove-Item -Recurse -Force $layout }
New-Item -ItemType Directory -Force (Join-Path $layout 'Assets') | Out-Null
Copy-Item (Join-Path $root 'bin\VagiiMouse.exe') $layout

$m = Get-Content (Join-Path $PSScriptRoot 'AppxManifest.xml') -Raw
$esc = { param($s) [Security.SecurityElement]::Escape($s) }
$m = $m.Replace('$NAME$', (& $esc $Name)).Replace('$PUBLISHER$', (& $esc $Publisher)).
  Replace('$PUBLISHERDISPLAY$', (& $esc $PublisherDisplayName)).Replace('$VERSION$', $ver)
[IO.File]::WriteAllText((Join-Path $layout 'AppxManifest.xml'), $m, (New-Object Text.UTF8Encoding $false))

# Logos: the same mouse glyph as res\app.ico, centred on a transparent canvas.
Add-Type -AssemblyName System.Drawing
function Save-Logo([string]$file, [int]$W, [int]$H, [double]$frac) {
  $b = New-Object System.Drawing.Bitmap $W, $H
  $g = [System.Drawing.Graphics]::FromImage($b)
  $g.SmoothingMode = 'AntiAlias'
  $s = [Math]::Min($W, $H) * $frac
  $ox = ($W - $s) / 2; $oy = ($H - $s) / 2
  $l = $ox + $s * 0.24; $t = $oy + $s * 0.06; $w = $s * 0.52; $h = $s * 0.88; $r = $s * 0.26 * 2
  $p = New-Object System.Drawing.Drawing2D.GraphicsPath
  $p.AddArc($l, $t, $r, $r, 180, 90); $p.AddArc($l + $w - $r, $t, $r, $r, 270, 90)
  $p.AddArc($l + $w - $r, $t + $h - $r, $r, $r, 0, 90); $p.AddArc($l, $t + $h - $r, $r, $r, 90, 90); $p.CloseFigure()
  $br = New-Object System.Drawing.Drawing2D.LinearGradientBrush ([System.Drawing.PointF]::new(0, $t)), ([System.Drawing.PointF]::new(0, $t + $h)), ([System.Drawing.Color]::FromArgb(37, 99, 235)), ([System.Drawing.Color]::FromArgb(97, 59, 235))
  $g.FillPath($br, $p)
  $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([Math]::Max(1, $s * 0.045))
  $g.DrawLine($pen, $ox + $s * 0.5, $t, $ox + $s * 0.5, $oy + $s * 0.42); $g.DrawLine($pen, $l, $oy + $s * 0.42, $l + $w, $oy + $s * 0.42)
  $g.Dispose()
  $b.Save((Join-Path $layout "Assets\$file"), [System.Drawing.Imaging.ImageFormat]::Png); $b.Dispose()
}
Save-Logo 'StoreLogo.png' 50 50 1.0
Save-Logo 'Square44x44Logo.png' 44 44 1.0
Save-Logo 'Square150x150Logo.png' 150 150 0.6
Save-Logo 'Wide310x150Logo.png' 310 150 0.6

$kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
$makeappx = Get-ChildItem $kits -Directory | Where-Object Name -like '10.*' | Sort-Object { [version]$_.Name } -Descending |
  ForEach-Object { Join-Path $_.FullName 'x64\makeappx.exe' } | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $makeappx) { throw 'makeappx.exe not found (install the Windows SDK)' }

$out = Join-Path $root ("bin\VagiiMouse-{0}-{1}.msix" -f $ver, $(if ($Store) { 'store' } else { 'dev' }))
& $makeappx pack /o /h SHA256 /d $layout /p $out | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)" }
Write-Host "Built $out"
