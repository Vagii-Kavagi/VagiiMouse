; VagiiMouse-Setup.exe - pointer speed booster, single exe, per-user (no admin).
; Build: build-installer.ps1 (Src defaults to the folder above this script)
#define AppName "Vagii Mouse"
#define AppVer "1.1.0"
#ifndef Src
  #define Src AddBackslash(SourcePath) + ".."
#endif

[Setup]
AppId={{8C3F1B27-6A94-4E5D-A0B8-2F7D91C4E6A3}
AppName={#AppName}
AppVersion={#AppVer}
AppVerName={#AppName} {#AppVer}
AppPublisher=Vagii
DefaultDirName={localappdata}\Programs\VagiiMouse
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
OutputDir=Output
OutputBaseFilename=VagiiMouse-Setup
SetupIconFile={#Src}\res\app.ico
UninstallDisplayIcon={app}\VagiiMouse.exe
UninstallDisplayName={#AppName}
VersionInfoVersion={#AppVer}.0
VersionInfoDescription=Vagii Mouse setup
WizardStyle=modern
Compression=lzma2/ultra
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
CloseApplications=yes
RestartApplications=no

[Files]
Source: "{#Src}\bin\VagiiMouse.exe"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\Vagii Mouse"; Filename: "{app}\VagiiMouse.exe"; WorkingDir: "{app}"; Comment: "Pointer speed beyond Windows' 20 levels"
Name: "{userdesktop}\Vagii Mouse"; Filename: "{app}\VagiiMouse.exe"; WorkingDir: "{app}"

[Registry]
; the app writes this Run value itself ("Start with Windows"); remove it on uninstall
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueName: "VagiiMouse"; ValueType: none; Flags: uninsdeletevalue dontcreatekey

[Run]
Filename: "{app}\VagiiMouse.exe"; Description: "Start Vagii Mouse now"; Flags: nowait postinstall

[UninstallRun]
Filename: "{cmd}"; Parameters: "/c taskkill /im VagiiMouse.exe /f"; Flags: runhidden; RunOnceId: "KillVagiiMouse"

[UninstallDelete]
Type: filesandordirs; Name: "{localappdata}\VagiiMouse"
