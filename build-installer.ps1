# Builds the app, then VagiiMouse-Setup.exe with Inno Setup (output: ..\Installers\Output).
# Run from this folder:  powershell -ExecutionPolicy Bypass -File .\build-installer.ps1
$ErrorActionPreference = 'Stop'
& (Join-Path $PSScriptRoot 'build.ps1')
$iscc = Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'
if (-not (Test-Path $iscc)) { throw "Inno Setup not found at $iscc" }
$iss = Join-Path $PSScriptRoot 'installer\VagiiMouse.iss'
$out = New-Item -ItemType Directory -Force (Join-Path $PSScriptRoot '..\Installers\Output')
& $iscc /Q "/O$($out.FullName)" "/DSrc=$PSScriptRoot" $iss
if ($LASTEXITCODE -ne 0) { throw "ISCC failed ($LASTEXITCODE)" }
Copy-Item $iss (Join-Path $PSScriptRoot '..\Installers\VagiiMouse.iss') -Force
Get-Item (Join-Path $out.FullName 'VagiiMouse-Setup.exe') | Select-Object FullName, Length
