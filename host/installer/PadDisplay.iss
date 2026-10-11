; PadDisplay by Nezucloud. Copyright (c) 2026 Nezucloud.
; SPDX-License-Identifier: MIT
; The Windows installer (Inno Setup 6). Build it with build-installer.cmd, which builds host\dist\PadDisplay first.
; It copies the same files the portable zip holds into Program Files and then lets PadDisplay.exe do the setup, one step
; per ticked box (the --install-... commands in src\main.cpp), so the installer and the tray menu share one implementation.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#define Dist "..\dist\PadDisplay"

[Setup]
AppId={{EC87292E-C261-470E-BCD3-889A40014B5F}
AppName=PadDisplay
AppVersion={#AppVersion}
AppVerName=PadDisplay {#AppVersion}
AppPublisher=Nezucloud
AppPublisherURL=https://github.com/NezucloudResearch/PadDisplay
AppSupportURL=https://github.com/NezucloudResearch/PadDisplay/issues
VersionInfoVersion={#AppVersion}
DefaultDirName={autopf}\PadDisplay
DefaultGroupName=PadDisplay
DisableProgramGroupPage=yes
; The folder can exist before a first install: an uninstall leaves the virtual camera's file there until the next restart.
DirExistsWarning=no
UninstallDisplayIcon={app}\PadDisplay.exe
; Drivers, Program Files and the sign-in task all need administrator rights: one prompt, here.
PrivilegesRequired=admin
; PadDisplay.exe and the bundled drivers are 64-bit x64 builds; Windows 10 1809 is the oldest Windows PadDisplay supports.
ArchitecturesAllowed=x64os
ArchitecturesInstallIn64BitMode=x64os
MinVersion=10.0.17763
OutputDir=..\dist
OutputBaseFilename=PadDisplay-Setup-{#AppVersion}
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
; PadDisplay is closed by PrepareToInstall (it must take its virtual monitor and controllers away itself).
CloseApplications=no
SetupLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Types]
Name: "full"; Description: "Everything the tablet can use"
Name: "custom"; Description: "Choose what to install"; Flags: iscustom

[Components]
Name: "core"; Description: "PadDisplay"; Types: full custom; Flags: fixed
Name: "monitor"; Description: "Virtual monitor: the tablet as a second screen (Virtual Display Driver by MikeTheTech)"; Types: full custom
Name: "gamepad"; Description: "Game controllers on the tablet (ViGEmBus by Nefarius)"; Types: full custom
Name: "camera"; Description: "The tablet's camera as a webcam (PadDisplay Camera, Windows 11)"; Types: full custom; MinVersion: 10.0.22000
Name: "cable"; Description: "The tablet's microphone (VB-CABLE by VB-Audio, donationware, vb-cable.com)"; Types: full custom

[Tasks]
Name: "adminstart"; Description: "Start PadDisplay with &administrator rights when I sign in, without a prompt (the tablet can then use Task Manager and other administrator windows)"
Name: "trust"; Description: "&Trust SignPath Foundation, the signer of the virtual monitor driver, so Windows installs it without asking (Windows will then install any driver signed by SignPath Foundation without asking)"; Components: monitor
Name: "desktopicon"; Description: "Create a &desktop shortcut"; Flags: unchecked

[Files]
; Every bundled driver is copied whatever is ticked: the tray menu can still set one up later.
Source: "{#Dist}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\PadDisplay"; Filename: "{app}\PadDisplay.exe"
Name: "{group}\Tablet app (APK) and documents"; Filename: "{app}"
Name: "{autodesktop}\PadDisplay"; Filename: "{app}\PadDisplay.exe"; Tasks: desktopicon

[Run]
; With the sign-in task, start it through the task: that is the start with administrator rights and no prompt.
Filename: "{sys}\schtasks.exe"; Parameters: "/run /tn ""PadDisplay Autostart"""; Description: "Start PadDisplay"; Flags: runhidden nowait postinstall skipifsilent; Tasks: adminstart
Filename: "{app}\PadDisplay.exe"; Description: "Start PadDisplay"; Flags: nowait postinstall skipifsilent runasoriginaluser; Tasks: not adminstart

[UninstallRun]
; Before the files go: PadDisplay takes away what it added to Windows (see RunUninstallSetup in src\main.cpp).
Filename: "{app}\PadDisplay.exe"; Parameters: "--uninstall-setup"; Flags: runhidden waituntilterminated; RunOnceId: "PadDisplaySetup"

[UninstallDelete]
; The virtual camera lives here because the Windows camera service loads it (see InstallCamera in src\vcam.cpp).
Type: filesandordirs; Name: "{commonpf64}\PadDisplay\Camera"
Type: dirifempty; Name: "{app}"

[Code]
const
  WM_CLOSE = $0010;
  RunningMutex = 'Local\PadDisplayHost';

var
  FailedSteps: String;
  FirstStep: Boolean;

// Asks a running PadDisplay to exit the way its tray menu does, and waits until it has cleaned up and gone.
function ClosePadDisplay: Boolean;
var
  Window: HWND;
  Tries: Integer;
begin
  Window := FindWindowByClassName('PadDisplayTray');
  if Window <> 0 then
    PostMessage(Window, WM_CLOSE, 0, 0);
  for Tries := 1 to 150 do
  begin
    if not CheckForMutexes(RunningMutex) then
      Break;
    Sleep(100);
  end;
  Result := not CheckForMutexes(RunningMutex);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  Result := '';
  if not ClosePadDisplay then
    Result := 'PadDisplay is still running. Choose Exit in its tray menu, then start this setup again.';
end;

// One setup step is one run of PadDisplay.exe; they all write to install-driver.log in %LOCALAPPDATA%\PadDisplay.
procedure RunSetupStep(const Flag, What: String);
var
  Code: Integer;
  Parameters: String;
begin
  Parameters := Flag;
  if not FirstStep then
    Parameters := Parameters + ' --append-log';
  FirstStep := False;
  WizardForm.StatusLabel.Caption := 'Setting up: ' + What + '...';
  if (not Exec(ExpandConstant('{app}\PadDisplay.exe'), Parameters, '', SW_HIDE, ewWaitUntilTerminated, Code)) or (Code <> 0) then
  begin
    FailedSteps := FailedSteps + #13#10 + '  - ' + What;
    Log('PadDisplay setup step failed: ' + Flag + ' (exit code ' + IntToStr(Code) + ')');
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep <> ssPostInstall then
    Exit;
  FailedSteps := '';
  FirstStep := True;
  // The publisher first: it is what keeps Windows from asking when the virtual monitor driver goes in.
  if WizardIsTaskSelected('trust') then
    RunSetupStep('--trust-publishers', 'trusting the driver publisher');
  if WizardIsComponentSelected('monitor') then
    RunSetupStep('--install-vdd', 'the virtual monitor driver');
  if WizardIsComponentSelected('gamepad') then
    RunSetupStep('--install-vigem', 'the game controller driver (ViGEmBus)');
  if WizardIsComponentSelected('camera') then
    RunSetupStep('--install-camera', 'the virtual camera');
  if WizardIsComponentSelected('cable') then
    RunSetupStep('--install-cable', 'the virtual microphone (VB-CABLE)');
  if WizardIsTaskSelected('adminstart') then
    RunSetupStep('--install-autostart', 'the start with administrator rights at sign-in');
  if FailedSteps <> '' then
    SuppressibleMsgBox('PadDisplay is installed, but these parts could not be set up:' + FailedSteps + #13#10#13#10 +
      'The reason is in ' + ExpandConstant('{localappdata}\PadDisplay\install-driver.log') +
      '. You can try again from PadDisplay''s tray menu.', mbError, MB_OK, IDOK);
end;

function InitializeUninstall: Boolean;
begin
  Result := ClosePadDisplay;
  if not Result then
    SuppressibleMsgBox('PadDisplay is still running. Choose Exit in its tray menu, then uninstall again.', mbError, MB_OK, IDOK);
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep <> usPostUninstall then
    Exit;
  if SuppressibleMsgBox('Also delete your PadDisplay settings (the PIN, the pairing with your tablet and the logs)?',
       mbConfirmation, MB_YESNO or MB_DEFBUTTON2, IDNO) = IDYES then
    DelTree(ExpandConstant('{localappdata}\PadDisplay'), True, True, True);
end;
