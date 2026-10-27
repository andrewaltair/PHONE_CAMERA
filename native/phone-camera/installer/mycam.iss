; MyCam installer (Inno Setup 6). Build with installer\build.ps1, which compiles the PC side, fetches
; the UsbDk driver package and runs ISCC. Replaces pc\install.ps1 for end users.
;
; Code signing (optional): pass a sign tool to ISCC, e.g.
;   ISCC /DSIGN "/Smycam=signtool sign /fd sha256 /tr http://timestamp.digicert.com /td sha256 /a $f" mycam.iss

#ifndef AppVersion
  #define AppVersion "1.5.1"
#endif
#ifndef BuildDir
  #define BuildDir "..\pc\build-release\Release"
#endif
#define UsbDkMsi "UsbDk_1.0.22_x64.msi"

[Setup]
AppId={{5FB4B81E-8CDC-44C8-A79B-6370B21FED09}
AppName=PHONE_CAMERA
AppVersion={#AppVersion}
AppVerName=PHONE_CAMERA {#AppVersion}
AppPublisher=PHONE_CAMERA
DefaultDirName={autopf}\PHONE_CAMERA
DisableProgramGroupPage=yes
DisableDirPage=auto
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Windows 11 uses the Frame Server virtual camera API (MyCamVCam.dll); Windows 10 1809+ falls back to a
; DirectShow camera (MyCamVCamDShow.dll) — see docs/WINDOWS10-SUPPORT.md. IsWindows11 in [Code] below
; picks which one to register.
MinVersion=10.0.17763
OutputDir=output
OutputBaseFilename=PHONE_CAMERA-Setup-{#AppVersion}
SetupIconFile=..\pc\companion\res\app.ico
UninstallDisplayIcon={app}\PHONE_CAMERA.exe
UninstallDisplayName=PHONE_CAMERA
WizardStyle=modern
Compression=lzma2
SolidCompression=yes
; On Windows 10, MyCamVCamDShow.dll can be loaded into any app that opened the camera (Discord, OBS, a
; browser), not just the OS-managed Frame Server service MyCamVCam.dll uses on Windows 11. Do not close
; those hosts automatically: if a file is locked, Setup reports that the camera app must be closed/restarted.
CloseApplications=no
RestartApplications=no
; The Run entry for "Start with Windows" is per user (the companion's own toggle writes the same value).
UsedUserAreasWarning=no
#ifdef SIGN
SignTool=mycam
SignedUninstaller=yes
#endif

[Messages]
WelcomeLabel2=This will install PHONE_CAMERA {#AppVersion}, which turns your phone into a webcam: an Android phone over USB or Wi-Fi, or an iPhone through Safari.%n%nYour phone shows up as a camera called "PHONE_CAMERA" in Zoom, Teams, OBS, browsers and the Camera app.%n%nClose any app that is using PHONE_CAMERA before continuing. The installer will not stop another app or Windows Frame Server.%n%nStart with Windows is configured later from PHONE_CAMERA Settings in the original user session.

[Tasks]
Name: "usbdriver"; Description: "Install the Android USB driver (not needed for Wi-Fi or iPhone/Safari)"; Flags: unchecked

[Files]
Source: "{#BuildDir}\PHONE_CAMERA.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\PHONE_CAMERA.exe"; DestDir: "{tmp}"; Flags: dontcopy
Source: "{#BuildDir}\PHONE_CAMERA_libusb.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\PHONE_CAMERA_libusb.dll"; DestDir: "{tmp}"; Flags: dontcopy
Source: "{#BuildDir}\MyCamVCam.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#BuildDir}\MyCamVCamDShow.dll"; DestDir: "{app}"; Flags: ignoreversion
; Kept after install so the uninstaller can remove UsbDk if the user asks.
Source: "redist\{#UsbDkMsi}"; DestDir: "{app}\redist"; Flags: ignoreversion
Source: "redist\UsbDk-LICENSE.txt"; DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "..\pc\third_party\libusb\COPYING"; DestDir: "{app}\licenses"; DestName: "libusb-COPYING.txt"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}\licenses"; DestName: "MyCam-MIT-LICENSE.txt"; Flags: ignoreversion
Source: "..\..\..\LICENSE"; DestDir: "{app}\licenses"; DestName: "PHONE_CAMERA-MIT-LICENSE.txt"; Flags: ignoreversion
Source: "..\pc\third_party\qrcodegen\LICENSE"; DestDir: "{app}\licenses"; DestName: "qrcodegen-MIT-LICENSE.txt"; Flags: ignoreversion
Source: "..\app\src\main\assets\licenses\DejaVu-LICENSE.txt"; DestDir: "{app}\licenses"; DestName: "DejaVu-LICENSE.txt"; Flags: ignoreversion

[Run]
Filename: "{app}\PHONE_CAMERA.exe"; Parameters: "--settings"; Description: "Open PHONE_CAMERA now and configure startup"; \
    Flags: nowait postinstall skipifsilent runasoriginaluser; Check: InstallSucceeded

[UninstallRun]
Filename: "{app}\PHONE_CAMERA.exe"; Parameters: "--quit"; RunOnceId: "QuitPHONE_CAMERA"; Flags: runhidden waituntilterminated
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""PHONE_CAMERA phone driver"" /F"; RunOnceId: "DeleteDriverTask"; Flags: runhidden waituntilterminated
; Do not stop Frame Server or other camera hosts. If a DLL is still loaded, Windows will report that the
; user must close the camera app and retry/restart the PC.

[UninstallDelete]
Type: dirifempty; Name: "{app}"

[Code]
var
  InstallationFailed: Boolean;
  UsbDkNeedsRestart: Boolean;

function InstallSucceeded: Boolean;
begin
  Result := not InstallationFailed;
end;

function GetCustomSetupExitCode: Integer;
begin
  if InstallationFailed then Result := 1
  else if UsbDkNeedsRestart then Result := 3010
  else Result := 0;
end;

function UsbDkInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{sys}\UsbDkHelper.dll'));
end;

// Which virtual-camera backend to register: Frame Server (Windows 11, build 22000+) or the DirectShow
// fallback (Windows 10; see docs/WINDOWS10-SUPPORT.md). GetWindowsVersionEx reports the true build
// number regardless of app-compat shims.
function IsWindows11: Boolean;
var
  Version: TWindowsVersion;
begin
  GetWindowsVersionEx(Version);
  Result := Version.Build >= 22000;
end;

function ExecChecked(const FileName, Params, What: String; ShowError: Boolean): Boolean;
var
  Code: Integer;
begin
  Code := -1;
  Result := Exec(FileName, Params, '', SW_HIDE, ewWaitUntilTerminated, Code) and (Code = 0);
  if not Result then begin
    Log(What + ' (exit ' + IntToStr(Code) + ').');
    if ShowError and not WizardSilent then
      MsgBox(What + ' (exit ' + IntToStr(Code) + ').', mbError, MB_OK);
  end;
end;

function RegisterCameraBackend: Boolean;
var
  DllName, DllPath: String;
begin
  if IsWindows11 then DllName := 'MyCamVCam.dll' else DllName := 'MyCamVCamDShow.dll';
  DllPath := ExpandConstant('{app}\' + DllName);
  Result := ExecChecked(ExpandConstant('{sys}\regsvr32.exe'), '/s "' + DllPath + '"',
                        'Could not register the PHONE_CAMERA camera backend ' + DllName, True);
end;

function ConfigureFirewall: Boolean;
var
  RuleName, AddParams: String;
begin
  RuleName := 'PHONE_CAMERA iPhone camera';
  // Delete is best-effort: the rule may not exist on a first install. Add is blocking and checked.
  ExecChecked(ExpandConstant('{sys}\netsh.exe'), 'advfirewall firewall delete rule name="' + RuleName + '"', '', False);
  AddParams := 'advfirewall firewall add rule name="' + RuleName + '" dir=in action=allow ' +
               'program="' + ExpandConstant('{app}\PHONE_CAMERA.exe') + '" protocol=TCP localport=47900 ' +
               'profile=private enable=yes';
  Result := ExecChecked(ExpandConstant('{sys}\netsh.exe'), AddParams,
                        'Could not configure the PHONE_CAMERA private-network firewall rule', True);
end;

function RegisterDriverTask: Boolean;
begin
  Result := ExecChecked(ExpandConstant('{app}\PHONE_CAMERA.exe'), '--register-task',
                        'Could not register the PHONE_CAMERA USB driver task', True);
end;

procedure UnregisterCameraBackend(const DllName: String);
var
  DllPath: String;
begin
  DllPath := ExpandConstant('{app}\' + DllName);
  if FileExists(DllPath) then
    ExecChecked(ExpandConstant('{sys}\regsvr32.exe'), '/s /u "' + DllPath + '"',
                'Could not unregister the PHONE_CAMERA camera backend ' + DllName, True);
end;

procedure UnregisterCameraBackends;
begin
  UnregisterCameraBackend('MyCamVCam.dll');
  UnregisterCameraBackend('MyCamVCamDShow.dll');
end;

// Installs the UsbDk driver if it's missing. A plain [Run] entry can't check msiexec's exit code, so a
// silent failure here (seen after uninstalling an older PHONE_CAMERA without a restart in between — the prior
// driver removal can still be settling) used to leave USB broken with no explanation. One retry after a
// short pause covers that race; if it still fails, the user gets a clear next step instead of silence.
function InstallUsbDk: Boolean;
var
  ResultCode, Attempt: Integer;
  Params: String;
begin
  Result := True;
  if UsbDkInstalled then exit;
  Params := '/i "' + ExpandConstant('{app}\redist\{#UsbDkMsi}') + '" /qn /norestart';
  ResultCode := -1;
  for Attempt := 1 to 2 do begin
    if not Exec(ExpandConstant('{sys}\msiexec.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      ResultCode := -1;
    if ResultCode = 3010 then begin
      break; // Preserve reboot-required status even if UsbDkHelper.dll appeared already.
    end;
    if ResultCode = 0 then begin
      ResultCode := 0;
      break;
    end;
    if Attempt = 1 then Sleep(3000); // Give a just-prior uninstall's driver cleanup a moment to settle.
  end;
  if ResultCode = 3010 then
    begin
      UsbDkNeedsRestart := True;
      if not WizardSilent then
        MsgBox('PHONE_CAMERA needs to finish setting up the USB driver. Please restart your PC before connecting a phone over USB.' + #13#10#13#10 +
               'Wi-Fi and iPhone (Safari) don''t need a restart.', mbInformation, MB_OK);
    end
  else if ResultCode <> 0 then begin
    Result := False;
    if not WizardSilent then
      MsgBox('Setting up the USB driver didn''t finish (error ' + IntToStr(ResultCode) + '). Phones over USB won''t ' +
             'work until this is fixed — try restarting your PC and running this installer again.' + #13#10#13#10 +
             'Wi-Fi and iPhone (Safari) don''t need the USB driver.', mbError, MB_OK);
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if WizardIsTaskSelected('usbdriver') and not InstallUsbDk then
      InstallationFailed := True
    else if WizardIsTaskSelected('usbdriver') and not RegisterDriverTask then
      InstallationFailed := True
    else if not RegisterCameraBackend then
      InstallationFailed := True
    else if not ConfigureFirewall then
      InstallationFailed := True;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then begin
    if InstallationFailed then begin
      WizardForm.FinishedHeadingLabel.Caption := 'PHONE_CAMERA setup failed';
      WizardForm.FinishedLabel.Caption :=
        'The camera backend, private-network firewall rule, or USB driver task could not be configured.' + #13#10#13#10 +
        'Close any camera application and run the installer again. No post-install PHONE_CAMERA launch was performed.';
    end else if UsbDkNeedsRestart then begin
      WizardForm.FinishedLabel.Caption :=
        'PHONE_CAMERA is installed. Restart Windows manually when convenient to finish the Android USB driver setup.';
    end;
  end;
end;

procedure RunHidden(const FileName, Params: String);
var
  Code: Integer;
begin
  Exec(FileName, Params, '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

// Upgrades: ask only this package's companion to leave cleanly. Do not stop other applications or
// Windows' Frame Server service; Restart Manager can report a locked camera DLL to the user.
function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  // Use this setup's own companion for --quit: an older installed version may not know the flag and
  // would just start up (and setup would wait on it forever).
  ExtractTemporaryFile('PHONE_CAMERA_libusb.dll');
  ExtractTemporaryFile('PHONE_CAMERA.exe');
  RunHidden(ExpandConstant('{tmp}\PHONE_CAMERA.exe'), '--quit');
  Result := '';
end;

// UsbDk can be shared with other software (e.g. virtual machine USB redirection), so only remove it
// when the user says so.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    UnregisterCameraBackends;
    ExecChecked(ExpandConstant('{sys}\netsh.exe'),
                'advfirewall firewall delete rule name="PHONE_CAMERA iPhone camera"',
                'Could not remove the PHONE_CAMERA private-network firewall rule', True);
    if UsbDkInstalled and not UninstallSilent then
      if MsgBox('Also remove the UsbDk USB driver?' + #13#10#13#10 +
                'Choose No if other software on this PC uses UsbDk.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
        RunHidden(ExpandConstant('{sys}\msiexec.exe'), '/x "' + ExpandConstant('{app}\redist\{#UsbDkMsi}') + '" /qn /norestart');
  end;
end;
