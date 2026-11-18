# Lists the exe files in a folder with their versions, and running Vagii Mouse processes.
param([string]$Dir)
Get-ChildItem $Dir -Filter *.exe | ForEach-Object { "{0,-22} {1}" -f $_.Name, $_.VersionInfo.FileVersion }
Get-Process VagiiMouse -ErrorAction SilentlyContinue | ForEach-Object { "running: " + $_.Path }
