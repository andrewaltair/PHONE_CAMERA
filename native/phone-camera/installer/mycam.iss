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
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\MyCamVCam.dll"""; \
    StatusMsg: "Registering the PHONE_CAMERA camera..."; Flags: runhidden waituntilterminated; Check: IsWindows11
Filename: "{sys}\regsvr32.exe"; Parameters: "/s ""{app}\MyCamVCamDShow.dll"""; \
    StatusMsg: "Registering the PHONE_CAMERA camera..."; Flags: runhidden waituntilterminated; Check: not IsWindows11
; Lets new phones get Windows' WinUSB driver later without a UAC prompt (a SYSTEM task users may only start).
Filename: "{app}\PHONE_CAMERA.exe"; Parameters: "--register-task"; \
    StatusMsg: "Setting up phone drivers..."; Flags: runhidden waituntilterminated; Tasks: usbdriver
; The iPhone page (HTTPS on TCP 47900): allowed in on private networks only, so Windows never asks the user.
; Deleted first, so an upgrade doesn't add a second copy.
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""PHONE_CAMERA iPhone camera"""; \
    Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; \
    Parameters: "advfirewall firewall add rule name=""PHONE_CAMERA iPhone camera"" dir=in action=allow program=""{app}\PHONE_CAMERA.exe"" protocol=TCP localport=47900 profile=private enable=yes"; \
    StatusMsg: "Letting iPhones on your network reach PHONE_CAMERA..."; Flags: runhidden waituntilterminated
Filename: "{app}\PHONE_CAMERA.exe"; Parameters: "--settings"; Description: "Open PHONE_CAMERA now and configure startup"; \
    Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
Filename: "{app}\PHONE_CAMERA.exe"; Parameters: "--quit"; RunOnceId: "QuitPHONE_CAMERA"; Flags: runhidden waituntilterminated
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\MyCamVCam.dll"""; RunOnceId: "UnregisterPHONE_CAMERA"; Flags: runhidden waituntilterminated
; Unregister both regardless of which one is actually registered (DllUnregisterServer on the inactive
; DLL is a harmless no-op): simpler than tracking which backend an install picked, and correct across an
; in-place Windows 10 -> 11 upgrade that never re-ran this installer.
Filename: "{sys}\regsvr32.exe"; Parameters: "/s /u ""{app}\MyCamVCamDShow.dll"""; RunOnceId: "UnregisterPHONE_CAMERA_DShow"; Flags: runhidden waituntilterminated
Filename: "{sys}\schtasks.exe"; Parameters: "/Delete /TN ""PHONE_CAMERA phone driver"" /F"; RunOnceId: "DeleteDriverTask"; Flags: runhidden waituntilterminated
Filename: "{sys}\netsh.exe"; Parameters: "advfirewall firewall delete rule name=""PHONE_CAMERA iPhone camera"""; RunOnceId: "DeleteFirewallRule"; Flags: runhidden waituntilterminated
; Do not stop Frame Server or other camera hosts. If a DLL is still loaded, Windows will report that the
; user must close the camera app and retry/restart the PC.

[UninstallDelete]
Type: dirifempty; Name: "{app}"

[Code]
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

// Installs the UsbDk driver if it's missing. A plain [Run] entry can't check msiexec's exit code, so a
// silent failure here (seen after uninstalling an older PHONE_CAMERA without a restart in between — the prior
// driver removal can still be settling) used to leave USB broken with no explanation. One retry after a
// short pause covers that race; if it still fails, the user gets a clear next step instead of silence.
procedure InstallUsbDk;
var
  ResultCode, Attempt: Integer;
  Params: String;
begin
  if UsbDkInstalled then exit;
  Params := '/i "' + ExpandConstant('{app}\redist\{#UsbDkMsi}') + '" /qn /norestart';
  ResultCode := -1;
  for Attempt := 1 to 2 do begin
    if not Exec(ExpandConstant('{sys}\msiexec.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      ResultCode := -1;
    if (ResultCode = 0) or UsbDkInstalled then begin
      ResultCode := 0;
      break;
    end;
    if Attempt = 1 then Sleep(3000); // Give a just-prior uninstall's driver cleanup a moment to settle.
  end;
  if WizardSilent then exit; // Don't pop dialogs during an unattended install.
  if ResultCode = 3010 then
    MsgBox('PHONE_CAMERA needs to finish setting up the USB driver. Please restart your PC before connecting a phone over USB.' + #13#10#13#10 +
           'Wi-Fi and iPhone (Safari) don''t need a restart.', mbInformation, MB_OK)
  else if ResultCode <> 0 then
    MsgBox('Setting up the USB driver didn''t finish (error ' + IntToStr(ResultCode) + '). Phones over USB won''t ' +
           'work until this is fixed — try restarting your PC and running this installer again.' + #13#10#13#10 +
           'Wi-Fi and iPhone (Safari) don''t need the USB driver.', mbError, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if (CurStep = ssPostInstall) and WizardIsTaskSelected('usbdriver') then InstallUsbDk;
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
  if (CurUninstallStep = usUninstall) and UsbDkInstalled and not UninstallSilent then
    if MsgBox('Also remove the UsbDk USB driver?' + #13#10#13#10 +
              'Choose No if other software on this PC uses UsbDk.', mbConfirmation, MB_YESNO or MB_DEFBUTTON2) = IDYES then
      RunHidden(ExpandConstant('{sys}\msiexec.exe'), '/x "' + ExpandConstant('{app}\redist\{#UsbDkMsi}') + '" /qn /norestart');
end;
