# Launches Vagii Mouse, captures only its window (PrintWindow), then kills it.
#   -Screen  captures the real on-screen pixels (needed for Mica/Acrylic, which PrintWindow can't see)
param([string]$Page = "summary", [string]$Out, [int]$Wait = 3500, [string]$Extra = "", [switch]$Screen, [string]$Exe, [switch]$Test)
if ($Test) { $env:VAGII_TEST = "1"; $cls = "VagiiMouseWndTest" } else { $cls = "VagiiMouseWnd" }
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  public struct RECT { public int L, T, R, B; }
}
"@
if (-not $Exe) { $Exe = Join-Path $PSScriptRoot "..\bin\VagiiMouse.exe" }
$p = Start-Process $Exe -ArgumentList ($(if ($Extra) { $Extra } else { "--tab=0" })) -PassThru
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $h = [W]::FindWindow($cls, [IntPtr]::Zero) }
if ($h -eq [IntPtr]::Zero) { Write-Host "window not found"; exit 1 }
Start-Sleep -Milliseconds $Wait
$r = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$r) | Out-Null
$bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
$g = [System.Drawing.Graphics]::FromImage($bmp)
if ($Screen) { [W]::SetForegroundWindow($h) | Out-Null; Start-Sleep -Milliseconds 600; $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size) }
else { $dc = $g.GetHdc(); [W]::PrintWindow($h, $dc, 2) | Out-Null; $g.ReleaseHdc($dc) }
$g.Dispose()
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png); $bmp.Dispose()
Stop-Process -Id $p.Id -Force

Write-Host "saved $Out ($($r.R - $r.L)x$($r.B - $r.T))"
