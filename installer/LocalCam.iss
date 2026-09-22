; Inno Setup script. Build the x64 app (build\) and the 32-bit camera DLL (build32\) first, then:
;   iscc /DAppVersion=1.0.0 installer\LocalCam.iss

#ifndef AppVersion
  #define AppVersion "0.0.0-dev"
#endif

[Setup]
AppId={{CAC36564-D33B-43D9-BA11-9F657731B69E}
AppName=LocalCam
AppVersion={#AppVersion}
AppPublisher=LocalCam contributors
DefaultDirName={autopf}\LocalCam
; Always Program Files: every user's video apps load the camera DLL from here, so users must not be able to write to it.
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
OutputBaseFilename=LocalCam-{#AppVersion}-setup
SetupIconFile=..\desktop\src\localcam.ico
UninstallDisplayIcon={app}\LocalCam.exe
LicenseFile=..\LICENSE
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; The sign-in entry goes to the user's own Run key, which the app's tray menu also toggles.
UsedUserAreasWarning=no

[Tasks]
Name: startup; Description: "Start LocalCam when I sign in"

[Files]
Source: "..\build\bin\Release\LocalCam.exe"; DestDir: "{app}"; Flags: ignoreversion
; The camera DLL is loaded by video apps: if one has it open, replace it after a restart.
Source: "..\build\bin\Release\softcam.dll"; DestDir: "{app}"; Flags: ignoreversion regserver restartreplace uninsrestartdelete
Source: "..\build32\bin\Release\softcam.dll"; DestDir: "{app}\x86"; Flags: ignoreversion regserver 32bit restartreplace uninsrestartdelete
Source: "..\LICENSE"; DestDir: "{app}"

[Icons]
Name: "{autoprograms}\LocalCam"; Filename: "{app}\LocalCam.exe"

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "LocalCam"; ValueData: """{app}\LocalCam.exe"" --tray"; Flags: uninsdeletevalue; Tasks: startup

[Run]
; Let phones reach the server on any network profile (new Wi-Fi networks default to Public on Windows 11), but only
; from the PC's own subnets. Only TLS connections carrying the pairing token get past the page.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""LocalCam"""; Flags: runhidden
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall add rule name=""LocalCam"" dir=in action=allow protocol=TCP program=""{app}\LocalCam.exe"" remoteip=localsubnet enable=yes"; Flags: runhidden
Filename: "{app}\LocalCam.exe"; Description: "Start LocalCam"; Flags: nowait postinstall skipifsilent

[UninstallRun]
Filename: "{sys}\taskkill.exe"; Parameters: "/f /im LocalCam.exe"; Flags: runhidden; RunOnceId: "StopLocalCam"
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""LocalCam"""; Flags: runhidden; RunOnceId: "DeleteFirewallRule"
