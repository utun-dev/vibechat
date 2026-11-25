#requires -Version 5.1
# Подготавливает MSYS2 UCRT64 и Inno Setup на чистой Windows, затем запускает существующие сборочные скрипты.
[CmdletBinding()]
param(
    [switch]$Full,
    [switch]$GuiOnly,
    [switch]$PrepareOnly,
    [switch]$UpdateTools,
    [ValidateRange(1, 256)][int]$Jobs = 4
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Import-BuildProxy {
    param([string]$Repository)
    $path = Join-Path $Repository 'proxy.sh'
    if (!(Test-Path -LiteralPath $path)) { return }
    $content = Get-Content -LiteralPath $path -Raw -Encoding UTF8
    $assignments = [regex]::Matches($content, "(?m)^\s*UTUN_HTTP_PROXY='([^'\r\n]*)'\s*(?:#.*)?$")
    if ($assignments.Count -ne 1) { throw "${path}: expected exactly one UTUN_HTTP_PROXY='http://host:port' assignment" }
    $proxy = $assignments[0].Groups[1].Value
    if (!$proxy) { return }
    $address = $null
    if (![Uri]::TryCreate($proxy, [UriKind]::Absolute, [ref]$address) -or $address.Scheme -notin @('http', 'https')) {
        throw "${path}: invalid HTTP proxy URL"
    }
    foreach ($name in @('http_proxy', 'https_proxy', 'HTTP_PROXY', 'HTTPS_PROXY')) {
        [Environment]::SetEnvironmentVariable($name, $proxy)
    }
    Write-Host "[INFO] HTTP-прокси загружен из ${path}: $($address.Host):$($address.Port)"
}

function Get-BuildDownload {
    param([string]$Url, [string]$Sha256, [string]$Cache)
    $path = Join-Path $Cache ([IO.Path]::GetFileName($Url))
    if (Test-Path -LiteralPath $path) {
        Write-Host "[INFO] Проверка SHA-256 ранее скачанного установщика: $path"
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -eq $Sha256) {
            Write-Host '[INFO] Контрольная сумма совпадает; повторное скачивание не требуется.'
            return $path
        }
        Write-Warning "Cached installer checksum mismatch; downloading again: $path"
        Remove-Item -LiteralPath $path
    }
    Write-Host "[INFO] Скачивание установщика: $Url"
    Write-Host "[INFO] Файл будет сохранён в $path. Ожидайте завершения загрузки."
    $partial = "$path.part"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    try {
        $request = @{ UseBasicParsing = $true; Uri = $Url; OutFile = $partial }
        $proxy = $env:https_proxy
        if (!$proxy) { $proxy = $env:HTTPS_PROXY }
        if (!$proxy) { $proxy = $env:http_proxy }
        if (!$proxy) { $proxy = $env:HTTP_PROXY }
        if ($proxy) {
            $address = $null
            if (![Uri]::TryCreate($proxy, [UriKind]::Absolute, [ref]$address) -or $address.Scheme -notin @('http', 'https')) {
                throw 'Invalid HTTP proxy URL in environment'
            }
            $endpoint = New-Object UriBuilder($address)
            if ($address.UserInfo) {
                $parts = $address.UserInfo -split ':', 2
                $username = [Uri]::UnescapeDataString($parts[0])
                $password = ''
                if ($parts.Count -eq 2) { $password = [Uri]::UnescapeDataString($parts[1]) }
                $credential = New-Object Net.NetworkCredential($username, $password)
                $request.ProxyCredential = New-Object Management.Automation.PSCredential($username, $credential.SecurePassword)
                $endpoint.UserName = ''
                $endpoint.Password = ''
            }
            $request.Proxy = $endpoint.Uri
        }
        Invoke-WebRequest @request
        Write-Host "[INFO] Загрузка завершена; проверка SHA-256: $Sha256"
        if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash -ne $Sha256) {
            throw "Downloaded installer SHA-256 mismatch: $Url"
        }
        Move-Item -LiteralPath $partial -Destination $path -Force
    } finally {
        if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial }
    }
    $megabytes = [Math]::Round((Get-Item -LiteralPath $path).Length / 1MB, 1)
    Write-Host "[INFO] Установщик проверен и сохранён: $megabytes МиБ, $([int]$timer.Elapsed.TotalSeconds) с."
    return $path
}

function Install-BuildTool {
    param([string]$Installer, [string[]]$Arguments)
    Write-Host "[INFO] Запуск установки: $Installer"
    Write-Host '[INFO] Установка выполняется автоматически; ожидайте завершения процесса.'
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process -FilePath $Installer -ArgumentList $Arguments -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "Installer failed (exit=$($process.ExitCode)): $Installer" }
    Write-Host "[INFO] Установщик завершил работу успешно; прошло $([int]$timer.Elapsed.TotalSeconds) с."
}

function Initialize-Msys2 {
    param([string]$MsysRoot, [string]$Cache)
    $bash = Join-Path $MsysRoot 'usr/bin/bash.exe'
    Write-Host "[INFO] Проверка MSYS2 в $MsysRoot — окружение Unix и менеджер пакетов для Windows."
    if (Test-Path -LiteralPath $bash) {
        Write-Host '[INFO] MSYS2 уже установлена; повторная установка пропускается.'
        return $bash
    }
    if (Test-Path -LiteralPath $MsysRoot) {
        throw "Directory exists without MSYS2 bash.exe: $MsysRoot. Select an empty location with MSYS2_ROOT."
    }
    $url = 'https://github.com/msys2/msys2-installer/releases/download/2026-09-27/msys2-x86_64-20260927.exe'
    $sha256 = '416d506c839a2da4bc184eeec8cbaaf1055ee698618228c62dfc4232a6970f0e'
    Write-Host "[INFO] MSYS2 не найдена. Будет установлена версия 2026-09-27 в $MsysRoot."
    $installer = Get-BuildDownload -Url $url -Sha256 $sha256 -Cache $Cache
    Install-BuildTool -Installer $installer -Arguments @('in', '--confirm-command', '--accept-messages', '--root', $MsysRoot)
    if (!(Test-Path -LiteralPath $bash)) { throw "MSYS2 installer did not create $bash" }
    Write-Host "[INFO] MSYS2 установлена; bash найден: $bash"
    return $bash
}

function Get-InnoCompilerVersion {
    param([string]$Compiler)
    Write-Host "[INFO] Проверка запуска ISCC без создания установщика: $Compiler"
    $probe = Join-Path ([IO.Path]::GetTempPath()) ('utun-inno-check-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $probe | Out-Null
    $process = $null
    try {
        $inputFile = Join-Path $probe 'check.iss'
        $script = "[Setup]`nAppName=uTun compiler check`nAppVersion=1.0`nDefaultDirName={tmp}\utun-compiler-check`n"
        $script += "CreateAppDir=no`nUninstallable=no`n"
        [IO.File]::WriteAllText($inputFile, $script, (New-Object Text.UTF8Encoding($false)))
        $process = New-Object Diagnostics.Process
        $process.StartInfo.FileName = $Compiler
        $process.StartInfo.Arguments = "/O- `"$inputFile`""
        $process.StartInfo.UseShellExecute = $false
        $process.StartInfo.CreateNoWindow = $true
        $process.StartInfo.RedirectStandardOutput = $true
        $process.StartInfo.RedirectStandardError = $true
        $process.StartInfo.StandardOutputEncoding = New-Object Text.UTF8Encoding($false)
        $process.StartInfo.StandardErrorEncoding = New-Object Text.UTF8Encoding($false)
        if (!$process.Start()) { throw "Cannot start ISCC: $Compiler" }
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(30000)) {
            $process.Kill()
            $process.WaitForExit()
            Write-Warning "ISCC compiler check timed out after 30 seconds: $Compiler"
            return $null
        }
        $output = $stdout.GetAwaiter().GetResult() + $stderr.GetAwaiter().GetResult()
        $match = [regex]::Match($output, '(?im)^Compiler engine version:\s*.*?\b(\d+\.\d+(?:\.\d+)?)\b')
        if ($process.ExitCode -eq 0 -and $match.Success) { return $match.Groups[1].Value }
        Write-Warning "ISCC compiler check failed: $Compiler (exit=$($process.ExitCode))`n$($output.Trim())"
        return $null
    } catch {
        Write-Warning "Cannot execute ISCC compiler check: $Compiler`n$($_.Exception.Message)"
        return $null
    } finally {
        if ($process -is [IDisposable]) { $process.Dispose() }
        Remove-Item -LiteralPath $probe -Recurse -Force
    }
}

function Initialize-InnoSetup {
    param([string]$Cache, [switch]$CheckOnly)
    $installDir = Join-Path $Cache 'InnoSetup'
    Write-Host '[INFO] Поиск Inno Setup 6.3+ — компилятора установщика VibeChat для Windows.'
    $candidates = @()
    if ($env:ISCC) {
        if (!(Test-Path -LiteralPath $env:ISCC)) { throw "ISCC does not point to an existing compiler: $env:ISCC" }
        $candidates += $env:ISCC
    }
    $candidates += Join-Path $installDir 'ISCC.exe'
    foreach ($variable in @('ProgramFiles', 'ProgramFiles(x86)', 'LOCALAPPDATA')) {
        $base = [Environment]::GetEnvironmentVariable($variable)
        if (!$base) { continue }
        if ($variable -eq 'LOCALAPPDATA') { $base = Join-Path $base 'Programs' }
        foreach ($version in @(7, 6)) { $candidates += Join-Path $base "Inno Setup $version/ISCC.exe" }
    }
    foreach ($candidate in $candidates) {
        if (!(Test-Path -LiteralPath $candidate)) { continue }
        $version = Get-InnoCompilerVersion -Compiler $candidate
        if (!$version) {
            if ($env:ISCC -and $candidate -eq $env:ISCC) { throw "ISCC compiler check failed: $candidate" }
            continue
        }
        if ([version]$version -ge [version]'6.3') {
            Write-Host "[INFO] Найден Inno Setup $version`: $candidate. Повторная установка пропускается."
            return $candidate
        }
        Write-Warning "Inno Setup 6.3+ is required; incompatible compiler: $candidate (version=$version)"
        if ($env:ISCC -and $candidate -eq $env:ISCC) { throw 'Remove or correct ISCC before retrying' }
    }
    if ($CheckOnly) { throw 'Inno Setup 6.3+ is missing. Run build.bat -PrepareOnly to install build tools explicitly.' }
    $url = 'https://github.com/jrsoftware/issrc/releases/download/is-7_1_1/innosetup-7.1.1-x64.exe'
    $sha256 = '8a5a6711c3766a6e5fb74f280ba0e3551e719ce94538d901fdbd1f8f9920b3f0'
    Write-Host "[INFO] Подходящий Inno Setup не найден. Будет установлена версия 7.1.1 в $installDir."
    Write-Host '[INFO] Inno Setup устанавливается для текущего пользователя.'
    $installer = Get-BuildDownload -Url $url -Sha256 $sha256 -Cache $Cache
    Install-BuildTool -Installer $installer -Arguments @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/SP-',
        '/CURRENTUSER', "/DIR=`"$installDir`"")
    $compiler = Join-Path $installDir 'ISCC.exe'
    if (!(Test-Path -LiteralPath $compiler)) { throw "Inno Setup installer did not create $compiler" }
    $version = Get-InnoCompilerVersion -Compiler $compiler
    if (!$version -or [version]$version -lt [version]'6.3') { throw "Installed Inno Setup compiler check failed: $compiler" }
    Write-Host "[INFO] Inno Setup установлен; компилятор $version проверен: $compiler"
    return $compiler
}

function Invoke-MsysBuildStep {
    param([string]$Bash, [string]$Name, [string]$Command, [switch]$AllowFailure)
    Write-Host "[INFO] $Name"
    $timer = [Diagnostics.Stopwatch]::StartNew()
    # Передаём bash файл: вложенные кавычки команд не проходят через правила argv Windows PowerShell 5.1.
    $scriptFile = [IO.Path]::GetTempFileName()
    try {
        $utf8 = New-Object Text.UTF8Encoding($false)
        $proxySetup = 'if [ -f ./proxy.sh ]; then . ./proxy.sh; fi'
        [IO.File]::WriteAllText($scriptFile, "set -euo pipefail`nexec 2>&1`n$proxySetup`n$Command`n", $utf8)
        & $Bash '--login' ($scriptFile.Replace('\', '/')) | Out-Host
        $result = $LASTEXITCODE
        Write-Host "[INFO] Завершён этап '$Name': код=$result, время=$([int]$timer.Elapsed.TotalSeconds) с."
        if ($result -ne 0 -and !$AllowFailure) { throw "$Name failed (exit=$result)" }
        return $result
    } finally {
        Remove-Item -LiteralPath $scriptFile
    }
}

function Invoke-WindowsBuild {
    param([string]$Repository, [switch]$Full, [switch]$GuiOnly, [switch]$PrepareOnly, [switch]$UpdateTools, [int]$Jobs = 4)
    if ($env:OS -ne 'Windows_NT' -or ![Environment]::Is64BitOperatingSystem) { throw 'Windows x64 is required' }
    $architecture = $env:PROCESSOR_ARCHITECTURE
    if ($env:PROCESSOR_ARCHITEW6432) { $architecture = $env:PROCESSOR_ARCHITEW6432 }
    if ($architecture -ne 'AMD64') { throw "Windows x64 is required; architecture=$architecture" }
    if ($Full -and $GuiOnly) { throw '-Full and -GuiOnly cannot be combined' }
    $msysRoot = 'C:\msys64'
    if ($env:MSYS2_ROOT) { $msysRoot = $env:MSYS2_ROOT }
    if ($env:MSYS2) { $msysRoot = $env:MSYS2 }
    if (![IO.Path]::IsPathRooted($msysRoot) -or $msysRoot -match '\s|[^\x20-\x7e]') {
        throw "MSYS2 installation path must be absolute ASCII without spaces: $msysRoot"
    }
    if (!$env:LOCALAPPDATA) { throw 'LOCALAPPDATA is not set; cannot locate the build tools cache' }
    $cache = Join-Path $env:LOCALAPPDATA 'uTun/build-tools'
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    Push-Location -LiteralPath $Repository
    $transcript = $false
    $consoleEncoding = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = New-Object Text.UTF8Encoding($false)
        $env:PYTHONUTF8 = '1'
        $env:PYTHONIOENCODING = 'utf-8:backslashreplace'
        Start-Transcript -Path (Join-Path $Repository 'build_windows_setup.log') -Force | Out-Null
        $transcript = $true
        $mode = 'инкрементальная сборка ядра, GUI и установщика'
        if ($Full) { $mode = 'полная пересборка ядра, GUI и установщика' }
        if ($GuiOnly) { $mode = 'сборка GUI и установщика' }
        $stages = 7
        if ($PrepareOnly) { $mode = 'только подготовка инструментов, без сборки'; $stages = 6 }
        Write-Host "[INFO] uTun / VibeChat: $mode. Потоков сборки: $Jobs."
        Write-Host "[INFO] Исходники: $Repository"
        Write-Host "[INFO] Кеш установщиков: $cache"
        Write-Host "[INFO] Все этапы подготовки записываются в $Repository/build_windows_setup.log"
        Import-BuildProxy -Repository $Repository
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
        $ProgressPreference = 'SilentlyContinue'
        Write-Host "[1/$stages] Проверка и при необходимости установка MSYS2."
        $newMsys = !(Test-Path -LiteralPath (Join-Path $msysRoot 'usr/bin/bash.exe'))
        $bash = Initialize-Msys2 -MsysRoot $msysRoot -Cache $cache
        $env:MSYSTEM = 'UCRT64'
        $env:CHERE_INVOKING = '1'
        $env:MSYS2_PATH_TYPE = 'strict'
        Write-Host "[2/$stages] Инициализация профиля MSYS2 и окружения UCRT64."
        if ($newMsys) {
            Invoke-MsysBuildStep -Bash $bash -Name 'Initializing MSYS2 login environment' -Command 'true' | Out-Null
        } else {
            Write-Host '[INFO] Используется готовое окружение; отдельная инициализация пропущена.'
        }
        Write-Host "[3/$stages] Локальная проверка установленных пакетов; интернет не используется."
        $result = Invoke-MsysBuildStep -Bash $bash -Name 'Checking UCRT64 build dependencies' `
            -Command 'bash tools/chatgui/install_deps_win.sh --check' -AllowFailure
        if ($result -ne 0 -and $result -ne 3) { throw "Local dependency check failed (exit=$result)" }
        $missingPackages = $result -eq 3
        if ($missingPackages -and !$newMsys -and !$UpdateTools -and !$PrepareOnly) {
            throw 'Missing build dependencies are listed above. Run build.bat -PrepareOnly to install them; automatic package updates are disabled.'
        }
        $preparePackages = $newMsys -or $UpdateTools -or ($PrepareOnly -and $missingPackages)
        if ($preparePackages) {
            if ($UpdateTools) { Write-Host '[INFO] Обновление инструментов явно запрошено параметром -UpdateTools.' }
            elseif ($newMsys) { Write-Host '[INFO] Новая установка MSYS2: требуется первичная подготовка пакетов с доступом к сети.' }
            else { Write-Host '[INFO] Найдены недостающие пакеты: для их установки требуется подготовка MSYS2 с доступом к сети.' }
            Write-Host "[4/$stages] Обновление MSYS2 перед установкой пакетов."
            # Первый проход может завершиться вместе с runtime; второй в новом процессе обязан пройти успешно.
            $result = Invoke-MsysBuildStep -Bash $bash -Name 'Updating MSYS2 core packages' `
                -Command 'exec pacman --noconfirm -Syu' -AllowFailure
            if ($result -ne 0) { Write-Warning "Первый проход обновления завершился с кодом $result; повторяем в новой оболочке." }
            Invoke-MsysBuildStep -Bash $bash -Name 'Completing MSYS2 system update' -Command 'exec pacman --noconfirm -Syu' | Out-Null
            Write-Host "[5/$stages] Установка недостающих компиляторов, инструментов и библиотек."
            Invoke-MsysBuildStep -Bash $bash -Name 'Installing UCRT64 build dependencies' `
                -Command 'bash tools/chatgui/install_deps_win.sh' | Out-Null
        } else {
            Write-Host "[4/$stages] Обновление пакетов пропущено: окружение готово, -UpdateTools не задан."
            Write-Host "[5/$stages] Установка пакетов пропущена; готовое окружение проверено локально."
        }
        Write-Host "[6/$stages] Проверка и при необходимости установка Inno Setup для упаковки приложения."
        $env:ISCC = Initialize-InnoSetup -Cache $cache -CheckOnly:(!$newMsys -and !$PrepareOnly -and !$UpdateTools)
        if ($PrepareOnly) { Write-Host '[INFO] Окружение готово. Сборка пропущена по параметру -PrepareOnly.'; return }
        $command = "bash build.sh --installer -j$Jobs"
        $log = 'build_win.log'
        if ($Full) { $command = "bash build.sh --full --installer -j$Jobs"; $log = 'build_full.log' }
        if ($GuiOnly) { $command = "cd tools/chatgui && bash build.sh --installer -j$Jobs"; $log = 'build_chatgui.log' }
        Write-Host "[7/$stages] Запуск: $mode. Журнал сборки: $Repository/$log"
        Write-Host '[INFO] Сборочные скрипты подготовят исходные зависимости, скомпилируют приложение и создадут установщик.'
        Invoke-MsysBuildStep -Bash $bash -Name 'Building uTun and VibeChat installer' `
            -Command "$command 2>&1 | tee $log" | Out-Null
        Write-Host '[INFO] Сборка завершена. Установщик: tools/chatgui/build/installer/VibeChat-*-win64-setup.exe'
    } catch {
        Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
        throw
    } finally {
        if ($transcript) { Stop-Transcript | Out-Null }
        [Console]::OutputEncoding = $consoleEncoding
        Pop-Location
    }
}

if ($MyInvocation.InvocationName -ne '.') {
    try {
        Invoke-WindowsBuild -Repository (Split-Path -Parent $PSScriptRoot) -Full:$Full -GuiOnly:$GuiOnly `
            -PrepareOnly:$PrepareOnly -UpdateTools:$UpdateTools -Jobs $Jobs
    } catch {
        Write-Host "[ERROR] $($_.Exception.Message)" -ForegroundColor Red
        Write-Host '[ERROR] See build_windows_setup.log and the build log for details.'
        exit 1
    }
}
