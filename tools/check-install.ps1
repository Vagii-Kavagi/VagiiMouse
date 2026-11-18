# Reports whether Vagii Mouse is installed, running, set to start with Windows, and its settings.
Get-Process VagiiMouse -ErrorAction SilentlyContinue | Select-Object Id, Path | Format-Table -AutoSize
Get-ItemProperty HKCU:\Software\Microsoft\Windows\CurrentVersion\Run -Name VagiiMouse -ErrorAction SilentlyContinue | Select-Object VagiiMouse | Format-List
Get-Content "$env:LOCALAPPDATA\VagiiMouse\settings.ini" -ErrorAction SilentlyContinue
$exe = "$env:LOCALAPPDATA\Programs\VagiiMouse\VagiiMouse.exe"
if (Test-Path $exe) { "Installed version: " + (Get-Item $exe).VersionInfo.FileVersion }
Get-ChildItem "$env:TEMP\VagiiMouseUpdate" -ErrorAction SilentlyContinue | Select-Object Name, Length | Format-Table -AutoSize
