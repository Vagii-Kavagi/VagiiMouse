# Builds bin\VagiiMouse.exe (native C++, static CRT: no runtime needed).
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'C++ build tools not found' }
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$bin = Join-Path $root 'bin'; $obj = Join-Path $root 'obj'
New-Item -ItemType Directory -Force $bin, $obj | Out-Null
$common = "/nologo /std:c++20 /EHsc /O2 /MT /W3 /permissive- /DUNICODE /D_UNICODE /utf-8 /wd4996"
$rc = "rc /nologo /fo `"$obj\app.res`" `"$root\res\app.rc`""
$link = "/link /SUBSYSTEM:WINDOWS `"$obj\app.res`" user32.lib gdi32.lib comctl32.lib shell32.lib advapi32.lib d2d1.lib dwrite.lib dwmapi.lib winhttp.lib bcrypt.lib ole32.lib windowsapp.lib"
$cmd = "$rc && cl $common `"$root\src\main.cpp`" /Fo`"$obj\\`" /Fe`"$bin\VagiiMouse.exe`" $link"
$ErrorActionPreference = 'Continue'
cmd /c "`"$vcvars`" >nul 2>&1 && $cmd 2>&1"
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Write-Host "Built $bin\VagiiMouse.exe"
