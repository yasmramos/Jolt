; Inno Setup script for the Jolt development engine on Windows.
;
; Build with:
;   iscc /DAppVersion=0.1.0 /DStageDir=C:\path\to\stage packaging\windows\oskel.iss
;
; `StageDir` is the tree produced by `cmake --install --prefix stage`:
;   stage\bin\oskel.exe
;   stage\share\oskel\std\*.oskl
; The installer reproduces that layout under the install directory, so the
; engine resolves `import std.math` through `..\share\oskel\std` relative to the
; executable, with no environment variable needed.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef StageDir
  #define StageDir "..\..\stage"
#endif

#define AppName "Jolt"
#define AppPublisher "Jolt contributors"
#define AppUrl "https://github.com/yasmramos/Jolt"
#define AppExeName "oskel.exe"

[Setup]
; A fixed AppId ties every version to one product: an upgrade replaces the
; previous install instead of stacking a second entry in "Apps & features".
AppId={{ef996165-7773-4c09-914c-54e4183f16f7}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
AppUpdatesURL={#AppUrl}
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
LicenseFile=..\..\LICENSE
OutputDir=..\..\dist
OutputBaseFilename=oskel-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; The engine and its standard library live under Program Files, and PATH is
; edited in the machine environment, both of which need elevation.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Broadcast WM_SETTINGCHANGE after install and uninstall, so a PATH edit is
; picked up by newly started processes without a reboot or a logoff.
ChangesEnvironment=yes
UninstallDisplayIcon={app}\bin\{#AppExeName}
VersionInfoDescription={#AppName} setup
VersionInfoProductName={#AppName}
VersionInfoCompany={#AppPublisher}
VersionInfoCopyright=MIT License

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "addtopath"; Description: "Add Jolt to the system PATH"; GroupDescription: "Integration:"; Flags: checkedonce
Name: "associate"; Description: "Associate .oskl files with Jolt"; GroupDescription: "Integration:"; Flags: unchecked

[Files]
Source: "{#StageDir}\bin\{#AppExeName}"; DestDir: "{app}\bin"; Flags: ignoreversion
Source: "{#StageDir}\share\oskel\std\*"; DestDir: "{app}\share\oskel\std"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#AppName}"; Filename: "{app}\bin\{#AppExeName}"
Name: "{group}\Uninstall {#AppName}"; Filename: "{uninstallexe}"

[Registry]
; `.oskl` file association. `HKA` resolves to `HKLM` in this administrative
; install, so the association is machine-wide and does not land in the
; elevated user's hive (which is what a plain `HKCU` would do here, and what
; Inno warns about). `Software\Classes` is the documented spelling of `HKCR`.
; `uninsdeletevalue`/`uninsdeletekey` remove it on uninstall.
Root: HKA; Subkey: "Software\Classes\.oskl"; ValueType: string; ValueName: ""; ValueData: "Jolt.Source"; Flags: uninsdeletevalue; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Jolt.Source"; ValueType: string; ValueName: ""; ValueData: "Jolt source file"; Flags: uninsdeletekey; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Jolt.Source\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\bin\{#AppExeName},0"; Tasks: associate
Root: HKA; Subkey: "Software\Classes\Jolt.Source\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\bin\{#AppExeName}"" ""%1"""; Tasks: associate

[Run]
Filename: "{app}\bin\{#AppExeName}"; Parameters: "--version"; Description: "Check the Jolt installation"; Flags: postinstall nowait skipifsilent

[Code]
const
  EnvironmentKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';

{ True when `Path` is not already a whole entry of the machine PATH. The
  comparison wraps both sides in `;` so a partial match (C:\Jolt vs
  C:\Jolt\bin) is not mistaken for a full one, and is case-insensitive
  because Windows paths are. }
function NeedsAddPath(const Path: string): Boolean;
var
  Paths: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Path) + ';', ';' + Uppercase(Paths) + ';') = 0;
end;

procedure EnvAddPath(const Path: string);
var
  Paths: string;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
    Paths := '';
  if Paths = '' then
    Paths := Path
  else
    Paths := Paths + ';' + Path;
  if not RegWriteExpandStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
    Log('Could not add ' + Path + ' to the machine PATH');
end;

procedure EnvRemovePath(const Path: string);
var
  Paths: string;
  Target: string;
  P: Integer;
begin
  if not RegQueryStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
    exit;

  Target := ';' + Uppercase(Path) + ';';
  P := Pos(Target, ';' + Uppercase(Paths) + ';');
  if P = 0 then
    exit;

  { P indexes the string with a leading `;` added, so `Path` starts at P in
    that string and at P-1 in `Paths`. A leading entry (P = 1) has no separator
    before it, so the `;` that follows is removed instead. }
  if P = 1 then
    Delete(Paths, 1, Length(Path) + 1)
  else
    Delete(Paths, P - 1, Length(Path) + 1);

  if not RegWriteExpandStringValue(HKEY_LOCAL_MACHINE, EnvironmentKey, 'Path', Paths) then
    Log('Could not remove ' + Path + ' from the machine PATH');
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('addtopath') then
  begin
    if NeedsAddPath(ExpandConstant('{app}\bin')) then
      EnvAddPath(ExpandConstant('{app}\bin'));
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  { Remove the PATH entry whatever the install-time choice was: a leftover
    entry would point at a directory that no longer exists. }
  if CurUninstallStep = usPostUninstall then
    EnvRemovePath(ExpandConstant('{app}\bin'));
end;
