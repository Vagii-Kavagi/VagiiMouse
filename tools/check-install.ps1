# Reports whether Vagii Mouse is installed, running, set to start with Windows, and its settings.
Get-Process VagiiMouse -ErrorAction SilentlyContinue | Select-Object Id, Path | Format-Table -AutoSize
Get-ItemProperty HKCU:\Software\Microsoft\Windows\CurrentVersion\Run -Name VagiiMouse -ErrorAction SilentlyContinue | Select-Object VagiiMouse | Format-List
Get-Content "$env:LOCALAPPDATA\VagiiMouse\settings.ini" -ErrorAction SilentlyContinue
