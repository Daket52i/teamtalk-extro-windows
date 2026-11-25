; TeamTalk Extro - Inno Setup script
; Packs the assembled TeamTalkExtro client folder into a branded installer.

#ifndef ExtroVersion
  #define ExtroVersion "5.31.0"
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
CloseApplications=yes
RestartApplications=no
PrivilegesRequiredOverridesAllowed=dialog
ChangesAssociations=yes

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"
Name: "ru"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "quicklaunchicon"; Description: "{cm:CreateQuickLaunchIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#SrcDir}\*"; Excludes: "vc_redist.x64.exe"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SrcDir}\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall skipifsourcedoesntexist; Check: Is64BitInstallMode

[Icons]
Name: "{group}\TeamTalk Extro"; Filename: "{app}\TeamTalkExtro.exe"; WorkingDir: "{app}"
Name: "{group}\{cm:UninstallProgram,TeamTalk Extro}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\TeamTalk Extro"; Filename: "{app}\TeamTalkExtro.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{userappdata}\Microsoft\Internet Explorer\Quick Launch\TeamTalk Extro"; Filename: "{app}\TeamTalkExtro.exe"; WorkingDir: "{app}"; Tasks: quicklaunchicon

[Registry]
Root: HKA; Subkey: "Software\Classes\.tt"; ValueType: string; ValueName: ""; ValueData: "TeamTalkExtro"; Flags: uninsdeletevalue
Root: HKA; Subkey: "Software\Classes\TeamTalkExtro"; ValueType: string; ValueName: ""; ValueData: "TeamTalk Host Settings"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\TeamTalkExtro\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\TeamTalkExtro.exe"",0"
Root: HKA; Subkey: "Software\Classes\TeamTalkExtro\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\TeamTalkExtro.exe"" ""%1"""

Root: HKA; Subkey: "Software\Classes\tt"; ValueType: string; ValueName: ""; ValueData: "URL:TeamTalk Protocol"; Flags: uninsdeletekey
Root: HKA; Subkey: "Software\Classes\tt"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKA; Subkey: "Software\Classes\tt\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: """{app}\TeamTalkExtro.exe"",0"
Root: HKA; Subkey: "Software\Classes\tt\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\TeamTalkExtro.exe"" ""%1"""

[Run]
Filename: "{tmp}\vc_redist.x64.exe"; Parameters: "/install /quiet /norestart"; Check: NeedsVCRedist
Filename: "{app}\TeamTalkExtro.exe"; Description: "{cm:LaunchProgram,TeamTalk Extro}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[Code]
function IsVCRedistInstalled(): Boolean;
var
  Installed: Cardinal;
  Major: Cardinal;
begin
  Result := False;

  if RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Installed', Installed) then
  begin
    if Installed = 1 then
    begin
      if RegQueryDWordValue(HKLM, 'SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64', 'Major', Major) then
      begin
        if (Major >= 14) then
          Result := True;
      end;
    end;
  end;
end;

function NeedsVCRedist(): Boolean;
begin
  Result := Is64BitInstallMode and (not IsVCRedistInstalled()) and FileExists(ExpandConstant('{tmp}\vc_redist.x64.exe'));
end;
