#define AppName "obs-rist-media-source"
#define AppVersion "2.0.0"
#define TargetObsVersion "32.2.1"
#define CompatibleObsVersionPrefix "32.2."
#define PackageDir "..\dist\obs-rist-media-source-2.0.0-obs-32.2.1-windows-x64"

[Setup]
AppId={{8F47116A-75D5-4BE6-A727-055887BD91C9}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher=romestylez
AppPublisherURL=https://github.com/romestylez/obs-rist-media-source
AppSupportURL=https://github.com/romestylez/obs-rist-media-source/issues
AppUpdatesURL=https://github.com/romestylez/obs-rist-media-source/releases
DefaultDirName={code:GetDefaultObsPath}
AppendDefaultDirName=no
DirExistsWarning=no
DisableProgramGroupPage=yes
Uninstallable=yes
UninstallFilesDir={app}\data\obs-plugins\obs-rist-media-source\uninstall
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
WizardStyle=modern
Compression=lzma2/ultra64
SolidCompression=yes
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
LicenseFile=..\LICENSE
OutputDir=..\dist
OutputBaseFilename=obs-rist-media-source-{#AppVersion}-obs-{#TargetObsVersion}-windows-x64-setup
VersionInfoVersion={#AppVersion}.0
VersionInfoCompany=romestylez
VersionInfoDescription=RIST Media Source for OBS Studio
VersionInfoProductName={#AppName}
VersionInfoProductVersion={#AppVersion}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "german"; MessagesFile: "compiler:Languages\German.isl"

[Files]
Source: "{#PackageDir}\obs-studio\obs-plugins\64bit\obs-rist-media-source.dll"; DestDir: "{app}\obs-plugins\64bit"; Flags: ignoreversion
Source: "{#PackageDir}\obs-studio\data\obs-plugins\obs-rist-media-source\locale\de-DE.ini"; DestDir: "{app}\data\obs-plugins\obs-rist-media-source\locale"; Flags: ignoreversion
Source: "{#PackageDir}\obs-studio\data\obs-plugins\obs-rist-media-source\locale\en-US.ini"; DestDir: "{app}\data\obs-plugins\obs-rist-media-source\locale"; Flags: ignoreversion
Source: "{#PackageDir}\README.md"; DestDir: "{app}\data\obs-plugins\obs-rist-media-source"; Flags: ignoreversion
Source: "{#PackageDir}\LICENSE"; DestDir: "{app}\data\obs-plugins\obs-rist-media-source"; Flags: ignoreversion
Source: "{#PackageDir}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}\data\obs-plugins\obs-rist-media-source"; Flags: ignoreversion
Source: "{#PackageDir}\licenses\*"; DestDir: "{app}\data\obs-plugins\obs-rist-media-source\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs

[Code]
const
  ObsExecutable = 'bin\64bit\obs64.exe';

function IsValidObsPath(const Path: String): Boolean;
begin
  Result := FileExists(AddBackslash(Path) + ObsExecutable);
end;

function RegistryObsPath(): String;
var
  Candidate: String;
begin
  Result := '';
  if RegQueryStringValue(HKLM64,
       'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio',
       'InstallLocation', Candidate) and IsValidObsPath(Candidate) then
  begin
    Result := RemoveBackslashUnlessRoot(Candidate);
    Exit;
  end;

  if RegQueryStringValue(HKCU,
       'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\OBS Studio',
       'InstallLocation', Candidate) and IsValidObsPath(Candidate) then
  begin
    Result := RemoveBackslashUnlessRoot(Candidate);
    Exit;
  end;
end;

function GetDefaultObsPath(Param: String): String;
begin
  Result := RegistryObsPath();
  if Result = '' then
    Result := ExpandConstant('{autopf}\obs-studio');
end;

function ConfirmObsVersion(const Path: String): Boolean;
var
  DetectedVersion: String;
  MessageText: String;
begin
  Result := True;
  if not GetVersionNumbersString(AddBackslash(Path) + ObsExecutable, DetectedVersion) then
    Exit;

  if Pos('{#CompatibleObsVersionPrefix}', DetectedVersion) <> 1 then
  begin
    if ActiveLanguage = 'german' then
      MessageText :=
        'Diese Plugin-Version wurde fÅr OBS Studio {#TargetObsVersion} gebaut.' + #13#10 +
        'Gefundene Version: ' + DetectedVersion + #13#10#13#10 +
        'Die Installation kann inkompatibel sein. Trotzdem fortfahren?'
    else
      MessageText :=
        'This plugin build targets OBS Studio {#TargetObsVersion}.' + #13#10 +
        'Detected version: ' + DetectedVersion + #13#10#13#10 +
        'The installation may be incompatible. Continue anyway?';
    Result := MsgBox(MessageText, mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  MessageText: String;
begin
  Result := True;
  if CurPageID <> wpSelectDir then
    Exit;

  if not IsValidObsPath(WizardDirValue) then
  begin
    if ActiveLanguage = 'german' then
      MessageText :=
        'Bitte wÑhle den OBS-Studio-Stammordner aus.' + #13#10#13#10 +
        'Im gewÑhlten Ordner wurde ' + ObsExecutable + ' nicht gefunden.'
    else
      MessageText :=
        'Please select the OBS Studio root directory.' + #13#10#13#10 +
        ObsExecutable + ' was not found in the selected directory.';
    MsgBox(MessageText, mbError, MB_OK);
    Result := False;
    Exit;
  end;

  Result := ConfirmObsVersion(WizardDirValue);
end;
