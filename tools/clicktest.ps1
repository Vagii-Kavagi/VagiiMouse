# Starts a test instance, posts clicks to its own window at DIP points, prints the saved level after each.
param([string]$Exe = (Join-Path $PSScriptRoot "..\bin\VagiiMouse.exe"), [string]$Page = "0")
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern IntPtr FindWindow(string c, IntPtr t);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
"@
$env:VAGII_TEST = "1"
$ini = "$env:LOCALAPPDATA\VagiiMouseTest\settings.ini"
$p = Start-Process $Exe -ArgumentList "--page=$Page" -PassThru
$h = [IntPtr]::Zero
for ($i = 0; $i -lt 50 -and $h -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 100; $h = [W]::FindWindow("VagiiMouseWndTest", [IntPtr]::Zero) }
Start-Sleep -Milliseconds 800
$s = [W]::GetDpiForWindow($h) / 96.0
$key = if ($Page -eq "1") { "tpLevel" } else { "level" }
function Level { (Select-String -Path $ini -Pattern "^$key=(\d+)").Matches[0].Groups[1].Value }
function ClickAt($x, $y, $name) {
  $lp = [IntPtr](([int]($y * $s) -shl 16) -bor [int]($x * $s))
  [W]::PostMessage($h, 0x200, [IntPtr]::Zero, $lp) | Out-Null; Start-Sleep -Milliseconds 80
  [W]::PostMessage($h, 0x201, [IntPtr]1, $lp) | Out-Null; Start-Sleep -Milliseconds 80
  [W]::PostMessage($h, 0x202, [IntPtr]::Zero, $lp) | Out-Null; Start-Sleep -Milliseconds 700
  "$name -> level $(Level)"
}
"start level $(Level)"
ClickAt 439 319 "plus"
ClickAt 439 319 "plus"
ClickAt 369 319 "minus"
Stop-Process -Id $p.Id -Force
