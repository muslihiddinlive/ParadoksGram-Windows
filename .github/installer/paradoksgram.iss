; Per-user installer for ParadoksGram (no admin rights needed).
; Defines passed by the workflow: Build, SourceExe, IconFile, OutDir.

[Setup]
AppId={{B7D1C0A2-5E1F-4F6A-9C0B-3D8E2A9F7C11}
AppName=ParadoksGram
AppVersion=build-{#Build}
AppPublisher=Paradoks Hub Community
DefaultDirName={localappdata}\Programs\ParadoksGram
DefaultGroupName=ParadoksGram
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=lowest
OutputDir={#OutDir}
OutputBaseFilename=ParadoksGram-Setup
SetupIconFile={#IconFile}
UninstallDisplayIcon={app}\Telegram.exe
Compression=lzma2
SolidCompression=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
CloseApplications=yes
RestartApplications=no
WizardStyle=modern

[Tasks]
Name: "desktopicon"; Description: "Desktop yorlig'ini yaratish"; Flags: checkedonce

[Files]
Source: "{#SourceExe}"; DestDir: "{app}"; DestName: "Telegram.exe"; Flags: ignoreversion

[Icons]
Name: "{autoprograms}\ParadoksGram"; Filename: "{app}\Telegram.exe"
Name: "{autodesktop}\ParadoksGram"; Filename: "{app}\Telegram.exe"; Tasks: desktopicon

[Run]
Filename: "{app}\Telegram.exe"; Description: "ParadoksGram ni ishga tushirish"; Flags: nowait postinstall skipifsilent
