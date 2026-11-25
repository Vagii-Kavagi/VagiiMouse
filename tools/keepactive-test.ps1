# Keep active, end to end: test instance with a 5 s idle threshold; samples Windows' idle clock
# for ~20 s and counts the app's nudges. If you touch the mouse/keyboard meanwhile, idle never
# reaches 5 s and no nudge is needed (rerun without touching anything).
param([int]$Seconds = 20)
$root = Split-Path $PSScriptRoot -Parent
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Idle {
  [StructLayout(LayoutKind.Sequential)] struct LII { public uint cb; public uint t; }
  [DllImport("user32.dll")] static extern bool GetLastInputInfo(ref LII p);
  public static uint Ms() { var l = new LII(); l.cb = 8; GetLastInputInfo(ref l); return (uint)Environment.TickCount - l.t; }
}
"@
$ini = Join-Path $env:LOCALAPPDATA 'VagiiMouseTest\settings.ini'
$t = Get-Content $ini -Raw
$t = [regex]::Replace($t, '(?m)^(keepActive|keepNudges)=.*\r?\n?', '')
Set-Content $ini ($t.TrimEnd() + "`r`nkeepActive=1`r`n") -Encoding Unicode -NoNewline
$env:VAGII_TEST = '1'; $env:VAGII_KEEP_TEST = '1'
$p = Start-Process (Join-Path $root 'bin\VagiiMouse.exe') -ArgumentList '--tray' -PassThru
# only count idle after the clock first resets (you may already have been idle for minutes)
$maxIdle = 0; $reset = $false
for ($i = 0; $i -lt $Seconds * 4; $i++) {
  Start-Sleep -Milliseconds 250; $m = [Idle]::Ms()
  if (-not $reset) { if ($m -lt 1500) { $reset = $true } ; continue }
  if ($m -gt $maxIdle) { $maxIdle = $m }
}
$p | Stop-Process -Force
$n = (Select-String -Path $ini -Pattern '^keepNudges=(\d+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }) -as [int]
"nudges sent: $n   longest idle seen: $([math]::Round($maxIdle / 1000, 1)) s (threshold 5 s, check every 1 s)"
if ($n -gt 0 -and $maxIdle -lt 7000) { "PASS: idle never got past ~6 s while you weren't touching anything" }
elseif ($n -eq 0 -and $maxIdle -lt 5000) { "INCONCLUSIVE: real input kept idle under 5 s; rerun without touching the mouse/keyboard" }
else { "FAIL" }
