#ifndef MyAppVersion
  #error MyAppVersion must be supplied by installer.ps1
#endif

#define MyAppName "VGPU-Mon"
#define MyCommandName "vgpu"
#define MyAppId "EE61F331-4A23-4740-B5B3-88EDDAA40122"

[Setup]
AppId={{{#MyAppId}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher=VGPU-Mon Project
AppCopyright=Copyright (c) 2026 VGPU-Mon contributors
DefaultDirName={localappdata}\Programs\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=auto
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
ChangesEnvironment=yes
CloseApplications=yes
RestartApplications=no
UsePreviousAppDir=yes
UsePreviousTasks=yes
LicenseFile=LICENSE
InfoAfterFile=README.md
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyCommandName}.exe
OutputDir=dist
OutputBaseFilename=VGPU-Mon-{#MyAppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
VersionInfoCompany=VGPU-Mon Project
VersionInfoDescription=VGPU-Mon Installer
VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}.0
VersionInfoVersion={#MyAppVersion}.0

[Tasks]
Name: "addtopath"; Description: "Add &{#MyCommandName} to my PATH"; GroupDescription: "Command line:"

[Files]
Source: "build\vgpu-mon.exe"; DestDir: "{app}"; DestName: "{#MyCommandName}.exe"; Flags: ignoreversion
Source: "README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "SECURITY.md"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyCommandName}.exe"; WorkingDir: "{%USERPROFILE}"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"

[Run]
Filename: "{app}\{#MyCommandName}.exe"; Description: "Launch {#MyAppName}"; WorkingDir: "{%USERPROFILE}"; Flags: nowait postinstall skipifsilent

[Code]
const
  EnvironmentKey = 'Environment';
  StateKey = 'Software\VGPU-Mon';

function NormalizePathToken(Value: String): String;
begin
  Result := Trim(Value);
  if (Length(Result) >= 2) and (Result[1] = '"') and
     (Result[Length(Result)] = '"') then
  begin
    Delete(Result, Length(Result), 1);
    Delete(Result, 1, 1);
  end;
  while (Length(Result) > 3) and
        ((Result[Length(Result)] = '\') or (Result[Length(Result)] = '/')) do
    Delete(Result, Length(Result), 1);
  Result := Lowercase(Result);
end;

function PathContains(const PathValue, Entry: String): Boolean;
var
  Remaining, Token: String;
  Separator: Integer;
  NormalizedEntry: String;
begin
  Result := False;
  Remaining := PathValue;
  NormalizedEntry := NormalizePathToken(Entry);
  while True do
  begin
    Separator := Pos(';', Remaining);
    if Separator = 0 then
    begin
      Token := Remaining;
      Remaining := '';
    end
    else
    begin
      Token := Copy(Remaining, 1, Separator - 1);
      Delete(Remaining, 1, Separator);
    end;
    if NormalizePathToken(Token) = NormalizedEntry then
    begin
      Result := True;
      Exit;
    end;
    if Separator = 0 then Exit;
  end;
end;

function AddPathEntry(const Entry: String): Boolean;
var
  PathValue, NewValue: String;
begin
  PathValue := '';
  RegQueryStringValue(HKCU, EnvironmentKey, 'Path', PathValue);
  if PathContains(PathValue, Entry) then
  begin
    Result := True;
    Exit;
  end;
  if PathValue = '' then
    NewValue := Entry
  else if PathValue[Length(PathValue)] = ';' then
    NewValue := PathValue + Entry
  else
    NewValue := PathValue + ';' + Entry;
  Result := RegWriteExpandStringValue(HKCU, EnvironmentKey, 'Path', NewValue);
  if Result then Log('Added VGPU-Mon to the user PATH.')
  else Log('ERROR: Could not add VGPU-Mon to the user PATH.');
end;

function RemovePathEntry(const Entry: String): Boolean;
var
  PathValue, Remaining, Token, NewValue: String;
  Separator: Integer;
  HaveToken: Boolean;
begin
  Result := True;
  if not RegQueryStringValue(HKCU, EnvironmentKey, 'Path', PathValue) then Exit;
  Remaining := PathValue;
  NewValue := '';
  HaveToken := False;
  while True do
  begin
    Separator := Pos(';', Remaining);
    if Separator = 0 then
    begin
      Token := Remaining;
      Remaining := '';
    end
    else
    begin
      Token := Copy(Remaining, 1, Separator - 1);
      Delete(Remaining, 1, Separator);
    end;
    if NormalizePathToken(Token) <> NormalizePathToken(Entry) then
    begin
      if HaveToken then NewValue := NewValue + ';';
      NewValue := NewValue + Token;
      HaveToken := True;
    end;
    if Separator = 0 then Break;
  end;
  if NewValue <> PathValue then
  begin
    Result := RegWriteExpandStringValue(HKCU, EnvironmentKey, 'Path', NewValue);
    if Result then Log('Removed VGPU-Mon from the user PATH.')
    else Log('ERROR: Could not remove VGPU-Mon from the user PATH.');
  end;
end;

procedure ConfigurePath;
var
  CurrentEntry, PreviousEntry, UserPath: String;
  Owned: Cardinal;
begin
  CurrentEntry := ExpandConstant('{app}');
  PreviousEntry := '';
  Owned := 0;
  RegQueryStringValue(HKCU, StateKey, 'PathEntry', PreviousEntry);
  RegQueryDWordValue(HKCU, StateKey, 'PathAdded', Owned);

  if (Owned = 1) and (PreviousEntry <> '') and
     (NormalizePathToken(PreviousEntry) <> NormalizePathToken(CurrentEntry)) then
    RemovePathEntry(PreviousEntry);

  if WizardIsTaskSelected('addtopath') then
  begin
    UserPath := '';
    RegQueryStringValue(HKCU, EnvironmentKey, 'Path', UserPath);
    if not PathContains(UserPath, CurrentEntry) then
    begin
      if AddPathEntry(CurrentEntry) then Owned := 1;
    end;
  end
  else
  begin
    if Owned = 1 then
    begin
      if PreviousEntry <> '' then RemovePathEntry(PreviousEntry)
      else RemovePathEntry(CurrentEntry);
    end;
    Owned := 0;
  end;

  RegWriteStringValue(HKCU, StateKey, 'PathEntry', CurrentEntry);
  RegWriteDWordValue(HKCU, StateKey, 'PathAdded', Owned);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then ConfigurePath;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  Entry: String;
  Owned: Cardinal;
begin
  if CurUninstallStep = usUninstall then
  begin
    Entry := ExpandConstant('{app}');
    Owned := 0;
    RegQueryStringValue(HKCU, StateKey, 'PathEntry', Entry);
    RegQueryDWordValue(HKCU, StateKey, 'PathAdded', Owned);
    if Owned = 1 then RemovePathEntry(Entry);
    RegDeleteValue(HKCU, StateKey, 'PathEntry');
    RegDeleteValue(HKCU, StateKey, 'PathAdded');
    RegDeleteKeyIfEmpty(HKCU, StateKey);
  end;
end;
