; Inno Setup script for the Voyeur PC port (compiled by tools/package.ps1).
;
; The program is portable: everything it writes (saves, settings, the disc
; location, the HD video pack, logs) goes into the folder it is installed to,
; never to AppData. So the default folder is a per-user one that needs no
; administrator rights (%LOCALAPPDATA%\Programs\Voyeur), and the folder can
; be moved or copied elsewhere afterwards.
;
; During setup the player points it at their own copy of the game (the
; folder holding the CD-i .cue/.bin, e.g. a "rom" folder). The choice is
; written to saves\disc_path.txt, which the game and the HD pack builder read.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef DistDir
  #define DistDir "..\dist\stage"
#endif
#ifndef OutDir
  #define OutDir "..\dist"
#endif

[Setup]
AppId={{6F0B6A3E-3C5B-4E7A-9C1D-56A1F0E2C9B7}
AppName=Voyeur (CD-i PC port)
AppVersion={#AppVersion}
AppVerName=Voyeur (CD-i PC port) {#AppVersion}
AppPublisher=voyeur-static-recomp
AppPublisherURL=https://github.com/tools81/voyeur-static-recomp
AppUpdatesURL=https://github.com/tools81/voyeur-static-recomp/releases
DefaultDirName={localappdata}\Programs\Voyeur
DefaultGroupName=Voyeur
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir={#OutDir}
OutputBaseFilename=Voyeur-{#AppVersion}-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\voyeur.exe
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

[Tasks]
Name: "desktopicon"; Description: "Create desktop shortcuts"; GroupDescription: "Shortcuts:"
Name: "hdpack"; Description: "Build the HD video pack after installing (AI upscaling of the game's video with Real-ESRGAN; needs a Vulkan GPU, about 10 hours and 5 GB; can be done later from the Start menu)"; GroupDescription: "HD edition:"; Flags: unchecked

[Files]
Source: "{#DistDir}\voyeur.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\voyeur_hd.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\hd_pack_builder.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\Build HD video pack.cmd"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#DistDir}\README.txt"; DestDir: "{app}"; Flags: ignoreversion isreadme
Source: "{#DistDir}\THIRD-PARTY.txt"; DestDir: "{app}"; Flags: ignoreversion

[Dirs]
Name: "{app}\saves"

[Icons]
Name: "{group}\Voyeur"; Filename: "{app}\voyeur.exe"; WorkingDir: "{app}"
Name: "{group}\Voyeur HD"; Filename: "{app}\voyeur_hd.exe"; WorkingDir: "{app}"
Name: "{group}\Build HD video pack"; Filename: "{app}\Build HD video pack.cmd"; WorkingDir: "{app}"
Name: "{group}\Read me"; Filename: "{app}\README.txt"
Name: "{group}\Uninstall Voyeur"; Filename: "{uninstallexe}"
Name: "{autodesktop}\Voyeur"; Filename: "{app}\voyeur.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autodesktop}\Voyeur HD"; Filename: "{app}\voyeur_hd.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\Build HD video pack.cmd"; WorkingDir: "{app}"; Description: "Build the HD video pack now"; Tasks: hdpack; Flags: postinstall shellexec
Filename: "{app}\voyeur.exe"; WorkingDir: "{app}"; Description: "Play Voyeur"; Flags: postinstall nowait skipifsilent unchecked

[UninstallDelete]
; files the program made itself; saves\ and hd_pack\ are left for the player to keep or delete
Type: filesandordirs; Name: "{app}\hd_work"
Type: filesandordirs; Name: "{app}\tools"
Type: files; Name: "{app}\saves\recomp.log"
Type: filesandordirs; Name: "{app}\saves\update"

[Code]
var
  DiscPage: TInputDirWizardPage;

function FindImage(Dir: String; var Image: String): Boolean;
var
  R: TFindRec;
begin
  Result := False;
  if FindFirst(AddBackslash(Dir) + '*.cue', R) then begin
    Image := AddBackslash(Dir) + R.Name; Result := True; FindClose(R); exit;
  end;
  if FindFirst(AddBackslash(Dir) + '*.bin', R) then begin
    Image := AddBackslash(Dir) + R.Name; Result := True; FindClose(R);
  end;
end;

procedure InitializeWizard;
begin
  DiscPage := CreateInputDirPage(wpSelectDir,
    'Game disc', 'Where are your Voyeur game files?',
    'Voyeur runs from your own copy of the Philips CD-i disc: a disc image in .cue/.bin form ' +
    '(for example "Voyeur (USA).cue" and ".bin"). Select the folder that holds those files ' +
    '(often called "rom"), then click Next.' + #13#10#13#10 +
    'If the files are not on this PC yet, click Next anyway: the game asks for the disc the first ' +
    'time it starts.',
    False, '');
  DiscPage.Add('Folder with the .cue / .bin files:');
  { silent installs: setup.exe /VERYSILENT /DISC="D:\Games\Voyeur" }
  DiscPage.Values[0] := ExpandConstant('{param:DISC|}');
  if DiscPage.Values[0] = '' then DiscPage.Values[0] := ExpandConstant('{src}');
end;

function ShouldSkipPage(PageID: Integer): Boolean;
begin
  Result := (PageID = DiscPage.ID) and WizardSilent;
end;

function NextButtonClick(CurPageID: Integer): Boolean;
var
  Image, Dir: String;
begin
  Result := True;
  if (CurPageID = DiscPage.ID) and (Trim(DiscPage.Values[0]) <> '') then begin
    Dir := Trim(DiscPage.Values[0]);
    if FileExists(Dir) then exit;   { a .cue/.bin typed directly }
    if not (FindImage(Dir, Image) or FindImage(AddBackslash(Dir) + 'rom', Image)) then
      Result := MsgBox('No .cue or .bin file was found in' + #13#10 + Dir + #13#10#13#10 +
                       'Continue anyway? (The game will ask for the disc when it starts.)',
                       mbConfirmation, MB_YESNO) = IDYES;
  end;
end;

{ only a folder (or file) that really holds the image is saved, so an update
  that is clicked through keeps the disc location already in saves\ }
function DiscChosen: Boolean;
var
  Image, Dir: String;
begin
  Dir := Trim(DiscPage.Values[0]);
  Result := (Dir <> '') and (FileExists(Dir) or FindImage(Dir, Image) or FindImage(AddBackslash(Dir) + 'rom', Image));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and DiscChosen then begin
    ForceDirectories(ExpandConstant('{app}\saves'));
    SaveStringToFile(ExpandConstant('{app}\saves\disc_path.txt'), Trim(DiscPage.Values[0]) + #13#10, False);
  end;
end;
