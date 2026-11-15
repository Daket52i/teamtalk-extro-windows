; TeamTalk Extro - Inno Setup script
; Packs the assembled TeamTalkExtro client folder into a branded installer.

#ifndef ExtroVersion
  #define ExtroVersion "1.0.0"
#endif

#ifndef SrcDir
  #define SrcDir "..\..\..\dist\TeamTalkExtro"
#endif

[Setup]
AppId={{6F1C2B44-9A3E-4C7D-9B21-7E5D0A3C8F11}
AppName=TeamTalk Extro
AppVersion={#ExtroVersion}
VersionInfoVersion={#ExtroVersion}
AppVerName=TeamTalk Extro {#ExtroVersion}
AppPublisher=TeamTalk Extro
AppPublisherURL=https://github.com/Daket52i/teamtalk-extro-windows
AppSupportURL=https://github.com/Daket52i/teamtalk-extro-windows/issues
AppUpdatesURL=https://github.com/Daket52i/teamtalk-extro-windows/releases
DefaultDirName={autopf}\TeamTalk Extro
DefaultGroupName=TeamTalk Extro
DisableProgramGroupPage=yes
OutputDir=Output
OutputBaseFilename=TeamTalkExtro-{#ExtroVersion}-win64-setup
SetupIconFile=..\..\Client\qtTeamTalk\images\teamtalk.ico
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
ArchitecturesInstallIn64BitMode=x64compatible
ArchitecturesAllowed=x64compatible
UninstallDisplayIcon={app}\TeamTalkExtro.exe

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "quicklaunchicon"; Description: "{cm:CreateQuickLaunchIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SrcDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\TeamTalk Extro"; Filename: "{app}\TeamTalkExtro.exe"; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallProgram,TeamTalk Extro}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\TeamTalk Extro"; Filename: "{app}\TeamTalkExtro.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\TeamTalk Extro"; Filename: "{app}\TeamTalkExtro.exe"; WorkingDir: "{app}"; Tasks: quicklaunchicon

[Run]
Filename: "{app}\TeamTalkExtro.exe"; Description: "{cm:LaunchProgram,TeamTalk Extro}"; Flags: nowait postinstall skipifsilent
