#define MyAppName "VDI Client"
#define MyAppVersion "1.5.0"
#define MyAppPublisher "VDI"
#define MyAppURL "https://www.vdi.com"
#define MyAppExeName "VDIClient.exe"
#define MyAppIcon "app.ico"

[Setup]
AppId={{A1B2C3D4-E5F6-7890-ABCD-EF1234567890}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}

DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}

AllowNoIcons=yes
OutputDir=installer_output
OutputBaseFilename=VDIClient-Setup
Compression=lzma
SolidCompression=yes
WizardStyle=modern

PrivilegesRequired=admin

ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0

CreateAppDir=yes
DisableProgramGroupPage=no

Uninstallable=yes
UninstallDisplayName={#MyAppName}
UninstallDisplayIcon={app}\{#MyAppExeName}

SetupIconFile={#MyAppIcon}

VersionInfoProductName={#MyAppName}
VersionInfoProductVersion={#MyAppVersion}
VersionInfoCompany={#MyAppPublisher}
VersionInfoDescription=VDI Client
VersionInfoCopyright=Copyright (C) 2025 VDI
VersionInfoOriginalFilename=VDIClient-Setup.exe

[Tasks]
Name: "desktopicon"; Description: "Create a &desktop icon"

[Files]
Source: "build\Release\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion
Source: "build\Release\*.dll"; DestDir: "{app}"; Flags: ignoreversion

Source: "build\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs
Source: "build\Release\platforms\*"; DestDir: "{app}\platforms"; Flags: ignoreversion recursesubdirs
Source: "build\Release\styles\*"; DestDir: "{app}\styles"; Flags: ignoreversion recursesubdirs
Source: "build\Release\imageformats\*"; DestDir: "{app}\imageformats"; Flags: ignoreversion recursesubdirs
Source: "build\Release\networkinformation\*"; DestDir: "{app}\networkinformation"; Flags: ignoreversion recursesubdirs
Source: "build\Release\tls\*"; DestDir: "{app}\tls"; Flags: ignoreversion recursesubdirs
Source: "build\Release\generic\*"; DestDir: "{app}\generic"; Flags: ignoreversion recursesubdirs
Source: "build\Release\iconengines\*"; DestDir: "{app}\iconengines"; Flags: ignoreversion recursesubdirs

Source: "app.ico"; DestDir: "{app}"; Flags: ignoreversion

; UsbDk driver for USB redirection (bundled MSI)
Source: "build\drivers\UsbDk_1.0.22_x64.msi"; DestDir: "{app}\drivers"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app.ico"
Name: "{group}\Uninstall {#MyAppName}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; IconFilename: "{app}\app.ico"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "Launch {#MyAppName}"; Flags: nowait postinstall skipifsilent

[UninstallDelete]
Type: filesandordirs; Name: "{app}\drivers"
Type: filesandordirs; Name: "{app}"

[Code]
function GetUninstallString(): String;
var
  KeyName, ResultStr: String;
begin
  ResultStr := '';
  KeyName := 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#SetupSetting("AppId")}_is1';
  if RegQueryStringValue(HKLM, KeyName, 'UninstallString', ResultStr) then
    Result := ResultStr
  else
    Result := '';
end;

function IsAdmin: Boolean;
var
  ResultCode: Integer;
begin
  // 检查当前用户是否管理员
  Result := Exec('net', 'session', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) and (ResultCode = 0);
end;

function InitializeSetup(): Boolean;
var
  UninstallCmd: String;
  ResultCode: Integer;
begin
  Result := True;

  // 检查管理员权限
  if not IsAdmin then
  begin
    MsgBox('This installer requires administrator privileges. Please run as administrator.', mbError, MB_OK);
    Result := False;
    Exit;
  end;

  // 检查是否已安装旧版本
  if GetUninstallString() <> '' then
  begin
    if MsgBox('Detected existing installation. Do you want to uninstall it first?', mbConfirmation, MB_YESNO) = IDYES then
    begin
      UninstallCmd := RemoveQuotes(GetUninstallString());
      if Exec(UninstallCmd, '/SILENT /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, ResultCode) then
      begin
        // 等待旧版本卸载完成
        Sleep(2000);
      end;
    end
    else
    begin
      Result := False;
      Exit;
    end;
  end;
end;

// UsbDk driver detection — checks if the kernel driver is already installed
function IsUsbDkInstalled: Boolean;
begin
  Result := FileExists(ExpandConstant('{sys}\drivers\UsbDk.sys'));
end;

// Silently install bundled UsbDk driver after VDI Client files are deployed
procedure CurStepChanged(CurStep: TSetupStep);
var
  ResultCode: Integer;
  MsiPath: String;
begin
  if CurStep = ssPostInstall then
  begin
    if not IsUsbDkInstalled then
    begin
      MsiPath := ExpandConstant('{app}\drivers\UsbDk_1.0.22_x64.msi');
      if FileExists(MsiPath) then
      begin
        if Exec('msiexec.exe', '/i "' + MsiPath + '" /quiet /norestart', '',
                SW_HIDE, ewWaitUntilTerminated, ResultCode) then
        begin
          case ResultCode of
            0:    Log('UsbDk driver installed successfully.');
            3010: MsgBox('UsbDk driver installed. A system reboot is recommended '
                       + 'for USB redirection to work properly.', mbInformation, MB_OK);
          else
            Log(Format('UsbDk installation failed with exit code %d', [ResultCode]));
          end;
        end
        else
          Log('Failed to launch UsbDk MSI installer');
      end
      else
        Log('UsbDk MSI not found at: ' + MsiPath);
    end
    else
      Log('UsbDk driver already installed, skipping.');
  end;
end;
