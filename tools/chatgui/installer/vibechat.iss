; Параметры комплекта и версии передаёт package_win.sh, пользовательских данных в комплекте нет.
#ifndef AppVersion
  #error AppVersion must be supplied by package_win.sh
#endif
#ifndef PackageDir
  #error PackageDir must be supplied by package_win.sh
#endif

[Setup]
AppId={{3A06309E-A5D4-46A6-A946-298634697A22}
AppName=VibeChat
AppVersion={#AppVersion}
AppPublisher=uTun
DefaultDirName={autopf}\VibeChat
DisableDirPage=no
DisableProgramGroupPage=yes
UsePreviousTasks=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
PrivilegesRequired=admin
OutputBaseFilename=VibeChat-{#AppVersion}-win64-setup
; Умеренное сжатие и два параллельных блока ускоряют упаковку EXE/DLL.
Compression=lzma2/normal
LZMANumBlockThreads=2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\vibechat.exe
LicenseFile={#PackageDir}\licenses\LICENSE
CloseApplications=yes
CloseApplicationsFilter=*.exe,*.dll
RestartApplications=no
SetupLogging=yes
UninstallLogging=yes

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"
Name: "russian"; MessagesFile: "compiler:Languages\Russian.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "{cm:AutoStart}"; GroupDescription: "{cm:AutoStartGroup}"

[InstallDelete]
; Снятая при обновлении галочка удаляет только наш прежний ярлык автозагрузки.
Type: files; Name: "{autostartup}\VibeChat.lnk"; Tasks: not autostart

[Files]
Source: "{#PackageDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\VibeChat"; Filename: "{app}\vibechat.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\VibeChat"; Filename: "{app}\vibechat.exe"; WorkingDir: "{app}"; Tasks: desktopicon
Name: "{autostartup}\VibeChat"; Filename: "{app}\vibechat.exe"; Parameters: "--minimized"; WorkingDir: "{app}"; Tasks: autostart

[Registry]
Root: HKLM; Subkey: "Software\Classes\utun"; ValueType: string; ValueName: ""; ValueData: "URL:VibeChat Protocol"
Root: HKLM; Subkey: "Software\Classes\utun"; ValueType: string; ValueName: "URL Protocol"; ValueData: ""
Root: HKLM; Subkey: "Software\Classes\utun\DefaultIcon"; ValueType: string; ValueName: ""; ValueData: "{app}\vibechat.exe,0"
Root: HKLM; Subkey: "Software\Classes\utun\shell\open\command"; ValueType: string; ValueName: ""; ValueData: """{app}\vibechat.exe"" -- ""%1"""

[Run]
Filename: "{app}\vibechat.exe"; Description: "{cm:LaunchProgram,VibeChat}"; WorkingDir: "{app}"; Flags: nowait postinstall skipifsilent runasoriginaluser

[CustomMessages]
english.AutoStartGroup=Startup:
russian.AutoStartGroup=Автозагрузка:
english.AutoStart=Start VibeChat minimized when signing in to Windows (for all users)
russian.AutoStart=Запускать VibeChat свёрнутым при входе в Windows (для всех пользователей)
english.FirewallFailed=Could not configure the VibeChat firewall rule. See the installation log for details.
russian.FirewallFailed=Не удалось настроить правило брандмауэра VibeChat. Подробности в журнале установки.

[Code]
// Удаляем только правило нашего инсталлятора для конкретного установленного EXE.
procedure RemoveFirewallRule;
var
    Parameters: String;
    ResultCode: Integer;
begin
    Parameters := 'advfirewall firewall delete rule name="VibeChat (installer)" program=' + AddQuotes(ExpandConstant('{app}\vibechat.exe'));
    if not ExecAndLogOutput(ExpandConstant('{sys}\netsh.exe'), Parameters, '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode, nil) then
        Log(Format('WARNING: cannot run firewall rule removal: error=%d', [ResultCode]))
    else if ResultCode <> 0 then
        Log(Format('WARNING: firewall rule removal returned exit=%d; see netsh output above', [ResultCode]))
    else
        Log('Firewall: previous VibeChat rule removed');
end;

// После копирования EXE разрешаем входящие подключения; ошибка установки видна и в журнале, и в GUI.
procedure CurStepChanged(CurStep: TSetupStep);
var
    Parameters: String;
    ResultCode: Integer;
begin
    if CurStep <> ssPostInstall then Exit;
    Log('VibeChat: installed to ' + ExpandConstant('{app}') + '; user config, keys and history are preserved');
    if WizardIsTaskSelected('autostart') then
        Log('Startup: enabled with --minimized; shortcut=' + ExpandConstant('{autostartup}\VibeChat.lnk'))
    else
        Log('Startup: disabled; shortcut=' + ExpandConstant('{autostartup}\VibeChat.lnk'));
    RemoveFirewallRule;
    Parameters := 'advfirewall firewall add rule name="VibeChat (installer)" dir=in action=allow program=' +
                  AddQuotes(ExpandConstant('{app}\vibechat.exe')) + ' enable=yes profile=any';
    if not ExecAndLogOutput(ExpandConstant('{sys}\netsh.exe'), Parameters, '', SW_SHOWNORMAL, ewWaitUntilTerminated, ResultCode, nil) then begin
        Log(Format('ERROR: cannot run firewall rule creation: error=%d', [ResultCode]));
        RaiseException(CustomMessage('FirewallFailed'));
    end;
    if ResultCode <> 0 then begin
        Log(Format('ERROR: firewall rule creation failed: exit=%d', [ResultCode]));
        RaiseException(CustomMessage('FirewallFailed'));
    end;
    Log('Firewall: inbound VibeChat connections allowed for installed EXE');
end;

// Деинсталлятор освобождает своё правило, не удаляя LocalAppData и пользовательские файлы.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
    Command: String;
begin
    if CurUninstallStep = usUninstall then begin
        if RegQueryStringValue(HKLM, 'Software\Classes\utun\shell\open\command', '', Command) and
            (Command = AddQuotes(ExpandConstant('{app}\vibechat.exe')) + ' -- "%1"') then begin
            if RegDeleteKeyIncludingSubkeys(HKLM, 'Software\Classes\utun') then
                Log('VibeChat: own utun URL handler removed')
            else Log('WARNING: could not remove own utun URL handler');
        end;
        RemoveFirewallRule;
        Log('VibeChat: uninstalling program files; user config, keys and history are preserved');
    end;
end;
