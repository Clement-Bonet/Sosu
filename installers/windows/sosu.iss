; Installer of sosuc for Windows x64, built by Inno Setup 6 from the laid out installation of a
; release (bin\, lib\, packages\):
;   iscc /DVersion=0.6.0 /DSourceDir=sosu-0.6.0-windows-x64 /DOutputDir=. installers\windows\sosu.iss
; For one user, without administrator rights: into %LOCALAPPDATA%\Programs\Sosu, with bin\ added to
; the user's PATH; the uninstaller removes both. Unsigned, unless SignPath signs it afterwards
; (.github/workflows/release.yml): SmartScreen then asks, More info, then Run anyway.

#ifndef Version
  #define Version "0.0.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\build\dist"
#endif
#ifndef OutputDir
  #define OutputDir "."
#endif
; The four numbers of the file version: 0.6.0-rc.1 gives 0.6.0.0.
#define NumericVersion Copy(Version, 1, Pos("-", Version + "-") - 1)

[Setup]
AppId={{6BF2B48B-A0F9-4686-83A1-1EB2130C7BAD}
AppName=Sosu
AppVersion={#Version}
AppVerName=Sosu {#Version}
AppPublisher=Sosu
AppPublisherURL=https://github.com/Clement-Bonet/Sosu
AppSupportURL=https://github.com/Clement-Bonet/Sosu/issues
VersionInfoVersion={#NumericVersion}
DefaultDirName={localappdata}\Programs\Sosu
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
ChangesEnvironment=yes
UninstallDisplayIcon={app}\bin\sosuc.exe
OutputDir={#OutputDir}
OutputBaseFilename=sosu-{#Version}-windows-x64-setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern

[Messages]
FinishedLabel=Sosu is installed in [name/ver]. Open a new terminal: sosuc is in your PATH.%n%nsosuc needs MSVC to build programs: Visual Studio or its Build Tools, with the workload "Desktop development with C++". sosuc finds it by itself.

[InstallDelete]
; A new version replaces the old one whole: no file of the old library stays behind.
Type: filesandordirs; Name: "{app}\bin"
Type: filesandordirs; Name: "{app}\lib"
Type: filesandordirs; Name: "{app}\packages"

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: recursesubdirs createallsubdirs ignoreversion

[Code]
const
  EnvironmentKey = 'Environment';

function BinDir(): String;
begin
  Result := ExpandConstant('{app}\bin');
end;

{ Position of `Dir` in the list `Paths` written as ;a;b;c; (0 when it is not there). }
function PathPos(Paths, Dir: String): Integer;
begin
  Result := Pos(';' + Uppercase(Dir) + ';', ';' + Uppercase(Paths) + ';');
end;

procedure AddToPath();
var
  Paths: String;
begin
  if not RegQueryStringValue(HKCU, EnvironmentKey, 'Path', Paths) then
    Paths := '';
  if PathPos(Paths, BinDir()) > 0 then
    exit;
  if Paths = '' then
    Paths := BinDir()
  else if Copy(Paths, Length(Paths), 1) = ';' then
    Paths := Paths + BinDir()
  else
    Paths := Paths + ';' + BinDir();
  RegWriteExpandStringValue(HKCU, EnvironmentKey, 'Path', Paths);
end;

procedure RemoveFromPath();
var
  Paths: String;
  P: Integer;
begin
  if not RegQueryStringValue(HKCU, EnvironmentKey, 'Path', Paths) then
    exit;
  P := PathPos(Paths, BinDir());
  if P = 0 then
    exit;
  Paths := ';' + Paths + ';';
  Delete(Paths, P, Length(BinDir()) + 1);
  Paths := Copy(Paths, 2, Length(Paths) - 2);
  RegWriteExpandStringValue(HKCU, EnvironmentKey, 'Path', Paths);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    AddToPath();
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usPostUninstall then
    RemoveFromPath();
end;
