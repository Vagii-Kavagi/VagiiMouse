# Installs bin\VagiiMouse-*-dev.msix (unsigned, Windows 11), checks it runs from the package,
# checks HKCU/AppData writes reach the real registry/folder, then removes it and restarts the
# normal install. Run from a real terminal (not a sandboxed shell).
param([switch]$Keep)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$msix = Get-ChildItem (Join-Path $root 'bin') -Filter 'VagiiMouse-*-dev.msix' | Sort-Object LastWriteTime -Descending | Select-Object -First 1
"package: $($msix.Name)"
Get-Process VagiiMouse -ErrorAction SilentlyContinue | ForEach-Object { "stopping running copy: $($_.Path)"; $_ | Stop-Process -Force }
Start-Sleep -Milliseconds 800
# needs Developer Mode: unsigned packages with an exe are refused otherwise (0x80073D2B)
Add-AppxPackage -Register (Join-Path $root "objmsixAppxManifest.xml")
$pkg = Get-AppxPackage VagiiMouse.Dev
"installed: $($pkg.PackageFullName)"
"install dir: $($pkg.InstallLocation)"

$ini = Join-Path $env:LOCALAPPDATA 'VagiiMouse\settings.ini'
$before = (Get-Item $ini -ErrorAction SilentlyContinue).LastWriteTime
Start-Process "shell:AppsFolder\$($pkg.PackageFamilyName)!VagiiMouse"
Start-Sleep 4
$p = Get-Process VagiiMouse -ErrorAction SilentlyContinue
"running: $($p.Path)"
"settings.ini written by packaged app (real AppData): $((Get-Item $ini -ErrorAction SilentlyContinue).LastWriteTime -ne $before)"

# A process inside the package context writes to HKCU; with virtualization off it must land in the real hive.
Remove-Item 'HKCU:\Software\VagiiMsixProbe' -ErrorAction SilentlyContinue
Invoke-CommandInDesktopPackage -PackageFamilyName $pkg.PackageFamilyName -AppId VagiiMouse -Command 'reg.exe' -Args 'add HKCU\Software\VagiiMsixProbe /v ok /d 1 /f'
Start-Sleep 2
"HKCU write from package reached real registry: $(Test-Path 'HKCU:\Software\VagiiMsixProbe')"
Remove-Item 'HKCU:\Software\VagiiMsixProbe' -ErrorAction SilentlyContinue

& (Join-Path $PSScriptRoot 'capture.ps1') -Out (Join-Path $root 'obj\msix-running.png')

if (-not $Keep) {
  Get-Process VagiiMouse -ErrorAction SilentlyContinue | Stop-Process -Force
  Start-Sleep -Milliseconds 800
  Remove-AppxPackage $pkg.PackageFullName
  "removed test package"
  $installed = Join-Path $env:LOCALAPPDATA 'Programs\VagiiMouse\VagiiMouse.exe'
  if (Test-Path $installed) { Start-Process $installed -ArgumentList '--tray'; "restarted $installed" }
}
