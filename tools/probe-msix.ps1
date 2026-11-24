# Store build without unvirtualizedResources:
#  1. do HKCU writes made inside the package reach the real registry? (expected: no)
#  2. does reg.exe started by the packaged app reach it? (the mouse registry reset relies on this)
#  3. the app's real reset commands, aimed at a throwaway key: backup + every default value
# Registers obj\msix (Developer Mode), only touches HKCU\Software\VagiiMsixProbe, unregisters.
# Leaves the user's running Vagii Mouse and their real mouse settings alone.
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Add-AppxPackage -Register (Join-Path $root 'obj\msix\AppxManifest.xml')
$pkg = Get-AppxPackage VagiiMouse.Dev
"registered: $($pkg.PackageFullName)"
$probe = 'HKCU:\Software\VagiiMsixProbe'
Remove-Item $probe -Recurse -ErrorAction SilentlyContinue

Invoke-CommandInDesktopPackage -PackageFamilyName $pkg.PackageFamilyName -AppId VagiiMouse -Command 'reg.exe' -Args 'add HKCU\Software\VagiiMsixProbe /v plain /d 1 /f'
Start-Sleep 3
$v = Get-ItemProperty $probe -ErrorAction SilentlyContinue
"1. plain write from inside the package reached the real registry: $($null -ne $v.plain)"
Remove-Item $probe -Recurse -ErrorAction SilentlyContinue

$bak = Join-Path $root 'obj\probe-backup.reg'
Remove-Item $bak -ErrorAction SilentlyContinue
$exe = Join-Path $pkg.InstallLocation 'VagiiMouse.exe'
Invoke-CommandInDesktopPackage -PackageFamilyName $pkg.PackageFamilyName -AppId VagiiMouse -Command $exe -Args "--probe-breakaway `"$bak`""
Start-Sleep 6
$v = Get-ItemProperty $probe -ErrorAction SilentlyContinue
"2. reg.exe as direct child reached real registry: $($v.direct -eq '1')   cmd.exe -> reg.exe reached it: $($v.viacmd -eq '1')"

$expect = @{ ActiveWindowTracking='0'; Beep='No'; DoubleClickHeight='4'; DoubleClickSpeed='500'; DoubleClickWidth='4'; ExtendedSounds='No';
  MouseHoverHeight='4'; MouseHoverTime='400'; MouseHoverWidth='4'; MouseSensitivity='10'; MouseSpeed='1'; MouseThreshold1='6';
  MouseThreshold2='10'; MouseTrails='0'; SnapToDefaultButton='0'; SwapMouseButtons='0' }
$m = Get-ItemProperty "$probe\Mouse" -ErrorAction SilentlyContinue
$bad = @($expect.Keys | Where-Object { $m.$_ -ne $expect[$_] })
$curveX = ($m.SmoothMouseXCurve | ForEach-Object { $_.ToString('x2') }) -join ''
"3. backup written: $(Test-Path $bak)   backup has the old value: $((Test-Path $bak) -and ((Get-Content $bak -Raw) -match 'Seed'))"
"   default text values right: $($bad.Count -eq 0) $(if ($bad) { '(wrong: ' + ($bad -join ', ') + ')' })"
"   SmoothMouseXCurve: $curveX ($($m.SmoothMouseXCurve.Count) bytes)   Y: $($m.SmoothMouseYCurve.Count) bytes"
Remove-Item $probe -Recurse -ErrorAction SilentlyContinue
Remove-Item $bak -ErrorAction SilentlyContinue

Remove-AppxPackage $pkg.PackageFullName
"unregistered"
