#requires -Version 5.1
# Проверяет настоящий Windows bootstrap с подставленными сетью, установщиками и границей MSYS2.
param([string]$Bootstrap = (Join-Path $PSScriptRoot '../tools/build_windows.ps1'))
$ErrorActionPreference = 'Stop'
. $Bootstrap
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('utun-bootstrap-tests-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testRoot | Out-Null
$script:checks = 0
$script:downloads = 0
$script:networkFailure = $false
$script:installerFailure = 0
$script:installerCalls = @()
$script:compilerVersion = '7.1.1'
$script:compilerProbeExit = 0
$script:payload = [Text.Encoding]::UTF8.GetBytes('installer fixture')
$savedEnvironment = @{}
foreach ($name in @('OS', 'PROCESSOR_ARCHITECTURE', 'PROCESSOR_ARCHITEW6432', 'MSYS2', 'MSYS2_ROOT',
    'LOCALAPPDATA', 'ProgramFiles', 'ProgramFiles(x86)', 'ISCC', 'MSYSTEM', 'CHERE_INVOKING', 'MSYS2_PATH_TYPE', 'UTUN_BOOTSTRAP_RESULT',
    'http_proxy', 'https_proxy', 'HTTP_PROXY', 'HTTPS_PROXY', 'PYTHONUTF8', 'PYTHONIOENCODING')) {
    $savedEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
}

function Check {
    param([string]$Name, [bool]$Condition)
    if (!$Condition) { throw "FAIL: $Name; fixture: $testRoot" }
    $script:checks++
    Write-Host "PASS: $Name"
}

function Expect-Failure {
    param([string]$Name, [scriptblock]$Action, [string]$Message)
    $failed = $false
    try { & $Action | Out-Null } catch {
        if ($_.Exception.Message -notlike "*$Message*") { throw }
        $failed = $true
    }
    Check $Name $failed
}

function Invoke-WebRequest {
    param([switch]$UseBasicParsing, [string]$Uri, [string]$OutFile, [Uri]$Proxy, [Management.Automation.PSCredential]$ProxyCredential)
    $script:lastProxy = $Proxy
    $script:lastProxyCredential = $ProxyCredential
    $script:downloads++
    [IO.File]::WriteAllBytes($OutFile, $script:payload)
    if ($script:networkFailure) { throw 'Injected network failure' }
}

function Start-Process {
    param([string]$FilePath, [string[]]$ArgumentList, [switch]$Wait, [switch]$PassThru)
    $script:installerCalls += ,@($FilePath, $ArgumentList)
    if (!$script:installerFailure) {
        if ($FilePath -like '*msys2*') {
            $target = Join-Path $ArgumentList[-1] 'usr/bin/bash.exe'
        } else {
            $directory = $ArgumentList[-1].Substring(5).Trim('"')
            $target = Join-Path $directory 'ISCC.exe'
        }
        New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
        [IO.File]::WriteAllText($target, 'tool fixture')
    }
    return [pscustomobject]@{ ExitCode = $script:installerFailure }
}

function Get-InnoCompilerVersion {
    param([string]$Compiler)
    if ($script:compilerProbeExit -ne 0) {
        Write-Warning "Injected ISCC compiler failure (exit=$script:compilerProbeExit): $Compiler"
        return $null
    }
    return $script:compilerVersion
}

function Get-Item {
    param([string]$LiteralPath)
    if ($LiteralPath -like '*ISCC.exe') {
        return [pscustomobject]@{ VersionInfo = [pscustomobject]@{ ProductVersion = '0.0.0.0' } }
    }
    return Microsoft.PowerShell.Management\Get-Item -LiteralPath $LiteralPath
}

try {
    foreach ($name in @('ProgramFiles', 'ProgramFiles(x86)', 'LOCALAPPDATA')) {
        [Environment]::SetEnvironmentVariable($name, (Join-Path $testRoot $name))
    }
    $cache = Join-Path $testRoot 'profile with spaces/cache'
    New-Item -ItemType Directory -Path $cache -Force | Out-Null
    $payloadPath = Join-Path $testRoot 'payload'
    [IO.File]::WriteAllBytes($payloadPath, $script:payload)
    $hash = (Get-FileHash -LiteralPath $payloadPath -Algorithm SHA256).Hash
    $download = Get-BuildDownload -Url 'https://example.invalid/setup.exe' -Sha256 $hash -Cache $cache
    Check 'installer is downloaded and verified' ($script:downloads -eq 1 -and (Test-Path -LiteralPath $download))
    Get-BuildDownload -Url 'https://example.invalid/setup.exe' -Sha256 $hash -Cache $cache | Out-Null
    Check 'verified cache avoids network access' ($script:downloads -eq 1)
    [IO.File]::WriteAllText($download, 'corrupted')
    Get-BuildDownload -Url 'https://example.invalid/setup.exe' -Sha256 $hash -Cache $cache | Out-Null
    Check 'corrupted cache is downloaded again' ($script:downloads -eq 2)
    Expect-Failure 'wrong downloaded SHA-256 is rejected' {
        Get-BuildDownload -Url 'https://example.invalid/bad.exe' -Sha256 ('0' * 64) -Cache $cache
    } 'SHA-256 mismatch'
    Check 'bad download is not cached and partial file is removed' `
        (!(Test-Path (Join-Path $cache 'bad.exe')) -and !(Test-Path (Join-Path $cache 'bad.exe.part')))
    $script:networkFailure = $true
    Expect-Failure 'network failure is propagated' {
        Get-BuildDownload -Url 'https://example.invalid/network.exe' -Sha256 $hash -Cache $cache
    } 'network failure'
    Check 'interrupted download is removed' (!(Test-Path (Join-Path $cache 'network.exe.part')))
    $script:networkFailure = $false

    $proxyRepo = Join-Path $testRoot 'proxy-repository'
    New-Item -ItemType Directory -Path $proxyRepo | Out-Null
    [IO.File]::WriteAllText((Join-Path $proxyRepo 'proxy.sh'), "UTUN_HTTP_PROXY='http://user:secret%21@127.0.0.1:8123'`n")
    Import-BuildProxy -Repository $proxyRepo
    Check 'shell proxy profile exports HTTP and HTTPS environment' `
        ($env:http_proxy -eq 'http://user:secret%21@127.0.0.1:8123' -and $env:HTTPS_PROXY -eq $env:http_proxy)
    Get-BuildDownload -Url 'https://example.invalid/proxy.exe' -Sha256 $hash -Cache $cache | Out-Null
    Check 'Windows downloads explicitly use proxy and decode credentials' `
        ($script:lastProxy.Host -eq '127.0.0.1' -and $script:lastProxy.Port -eq 8123 -and !$script:lastProxy.UserInfo -and
        $script:lastProxyCredential.UserName -eq 'user' -and $script:lastProxyCredential.GetNetworkCredential().Password -eq 'secret!')
    [IO.File]::WriteAllText((Join-Path $proxyRepo 'proxy.sh'), "UTUN_HTTP_PROXY=''`n")
    Import-BuildProxy -Repository $proxyRepo
    Check 'blank proxy profile preserves inherited environment' ($env:http_proxy -eq 'http://user:secret%21@127.0.0.1:8123')
    foreach ($name in @('http_proxy', 'https_proxy', 'HTTP_PROXY', 'HTTPS_PROXY')) {
        [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name])
    }

    # Следующие проверки сохраняют реальные установочные функции, заменяя только загрузку бинарных установщиков.
    function Get-BuildDownload {
        param([string]$Url, [string]$Sha256, [string]$Cache)
        $script:downloads++
        if ($script:networkFailure) { throw 'Network is unavailable' }
        Check 'installer URL is HTTPS and SHA-256 is pinned' ($Url.StartsWith('https://') -and $Sha256 -match '^[a-f0-9]{64}$')
        $path = Join-Path $Cache ([IO.Path]::GetFileName($Url))
        [IO.File]::WriteAllText($path, 'installer fixture')
        return $path
    }
    $msys = Join-Path $testRoot 'msys64'
    $bash = Initialize-Msys2 -MsysRoot $msys -Cache $cache
    Check 'MSYS2 is installed unattended into selected location' `
        ((Test-Path -LiteralPath $bash) -and $script:installerCalls[-1][1][0] -eq 'in')
    $before = $script:installerCalls.Count
    Initialize-Msys2 -MsysRoot $msys -Cache $cache | Out-Null
    Check 'existing MSYS2 is reused' ($script:installerCalls.Count -eq $before)
    $incomplete = Join-Path $testRoot 'incomplete'
    New-Item -ItemType Directory -Path $incomplete | Out-Null
    Expect-Failure 'unrelated or incomplete directory is preserved' {
        Initialize-Msys2 -MsysRoot $incomplete -Cache $cache
    } 'Directory exists without MSYS2'
    $script:installerFailure = 9
    Expect-Failure 'MSYS2 installer error is propagated' {
        Initialize-Msys2 -MsysRoot (Join-Path $testRoot 'failed-msys') -Cache $cache
    } 'Installer failed (exit=9)'
    $script:installerFailure = 0
    $env:ISCC = ''
    $inno = Initialize-InnoSetup -Cache $cache
    Check 'Inno Setup installs for current user with quoted path' `
        ((Test-Path -LiteralPath $inno) -and $script:installerCalls[-1][1] -contains '/CURRENTUSER' -and
        $script:installerCalls[-1][1][-1] -like '/DIR="*profile with spaces*"')
    $env:ISCC = $inno
    $before = $script:installerCalls.Count
    Initialize-InnoSetup -Cache $cache | Out-Null
    Check 'existing compatible Inno compiler is reused' ($script:installerCalls.Count -eq $before)
    Check 'ISCC with zero file ProductVersion is accepted using its working compiler engine' `
        ((Get-Item -LiteralPath $inno).VersionInfo.ProductVersion -eq '0.0.0.0' -and $env:ISCC -eq $inno)
    $script:compilerProbeExit = 2
    Expect-Failure 'a failing compiler is rejected even when it reports a valid version' {
        Initialize-InnoSetup -Cache $cache
    } 'ISCC compiler check failed'
    $script:compilerProbeExit = 0
    $userCompiler = Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 7/ISCC.exe'
    New-Item -ItemType Directory -Path (Split-Path -Parent $userCompiler) -Force | Out-Null
    [IO.File]::WriteAllText($userCompiler, 'compiler fixture')
    $env:ISCC = ''
    $found = Initialize-InnoSetup -Cache (Join-Path $testRoot 'another-cache')
    Check 'current-user Inno installation is discovered without reinstalling' `
        ($found -eq $userCompiler -and $script:installerCalls.Count -eq $before)
    $env:ISCC = $inno
    $script:compilerVersion = '6.2.0'
    Expect-Failure 'incompatible explicit compiler is rejected' { Initialize-InnoSetup -Cache $cache } 'correct ISCC'
    $script:compilerVersion = '7.1.1'
    $env:ISCC = ''
    Remove-Item -LiteralPath $userCompiler
    $beforeDownloads = $script:downloads
    Expect-Failure 'check-only does not download a missing Inno compiler' {
        Initialize-InnoSetup -Cache (Join-Path $testRoot 'missing-inno') -CheckOnly
    } 'Inno Setup 6.3+ is missing'
    Check 'missing Inno compiler does not trigger downloads' ($script:downloads -eq $beforeDownloads)
    $env:ISCC = $inno

    if (Test-Path -LiteralPath '/bin/bash') {
        $env:UTUN_BOOTSTRAP_RESULT = Join-Path $testRoot 'native bridge result.txt'
        $nativeResult = Invoke-MsysBuildStep -Bash '/bin/bash' -Name 'Native argument transfer' `
            -Command 'printf "%s" "quotes, spaces and Юникод" > "$UTUN_BOOTSTRAP_RESULT"'
        Check 'native shell file preserves quotes and UTF-8' `
            ($nativeResult -eq 0 -and [IO.File]::ReadAllText($env:UTUN_BOOTSTRAP_RESULT) -eq 'quotes, spaces and Юникод')
        $template = Join-Path (Split-Path -Parent $Bootstrap) '../proxy.sh.example'
        $profile = [IO.File]::ReadAllText([IO.Path]::GetFullPath($template)).Replace("UTUN_HTTP_PROXY=''", "UTUN_HTTP_PROXY='http://127.0.0.1:8123'")
        [IO.File]::WriteAllText((Join-Path $proxyRepo 'proxy.sh'), $profile)
        Push-Location $proxyRepo
        try {
            Invoke-MsysBuildStep -Bash '/bin/bash' -Name 'Native proxy import' `
                -Command 'printf "%s" "$http_proxy|$https_proxy|$HTTP_PROXY|$HTTPS_PROXY" > "$UTUN_BOOTSTRAP_RESULT"' | Out-Null
        } finally { Pop-Location }
        Check 'MSYS shell sources proxy profile before running commands' `
            ([IO.File]::ReadAllText($env:UTUN_BOOTSTRAP_RESULT) -eq ('http://127.0.0.1:8123|' * 3 + 'http://127.0.0.1:8123'))
        Expect-Failure 'native shell failures propagate' {
            Invoke-MsysBuildStep -Bash '/bin/bash' -Name 'Native failure' -Command 'exit 17'
        } 'exit=17'
    }

    $script:steps = @()
    $script:failStep = ''
    $script:coreRestart = $false
    $script:dependencyStatus = 0
    function Invoke-MsysBuildStep {
        param([string]$Bash, [string]$Name, [string]$Command, [switch]$AllowFailure)
        $script:steps += [pscustomobject]@{ Name = $Name; Command = $Command; Directory = (Get-Location).Path }
        if ($Name -eq $script:failStep) { throw "Injected $Name failure" }
        if ($script:networkFailure -and ($Command -like '*pacman*' -or $Command -eq 'bash tools/chatgui/install_deps_win.sh')) {
            throw "Network is unavailable: $Command"
        }
        if ($Command -eq 'bash tools/chatgui/install_deps_win.sh --check') { return $script:dependencyStatus }
        if ($Name -eq 'Updating MSYS2 core packages' -and $script:coreRestart) { return 1 }
        return 0
    }
    $repo = Join-Path $testRoot 'repository with spaces'
    New-Item -ItemType Directory -Path $repo | Out-Null
    $env:OS = 'Windows_NT'
    $env:PROCESSOR_ARCHITECTURE = 'AMD64'
    $env:PROCESSOR_ARCHITEW6432 = ''
    $env:MSYS2 = ''
    $env:MSYS2_ROOT = $msys
    $env:LOCALAPPDATA = Join-Path $testRoot 'profile with spaces'
    $env:PYTHONUTF8 = '0'
    $env:PYTHONIOENCODING = 'cp1251:strict'
    $script:networkFailure = $true
    $beforeDownloads = $script:downloads
    $beforeInstallers = $script:installerCalls.Count
    Invoke-WindowsBuild -Repository $repo -Jobs 3
    Check 'Windows bootstrap overrides legacy Python pipe encoding with UTF-8' `
        ($env:PYTHONUTF8 -eq '1' -and $env:PYTHONIOENCODING -eq 'utf-8:backslashreplace')
    Check 'ready environment builds offline after one local package check' `
        ($script:steps.Count -eq 2 -and $script:steps[0].Command -eq 'bash tools/chatgui/install_deps_win.sh --check' -and
        $script:steps[-1].Command -eq 'bash build.sh --installer -j3 2>&1 | tee build_win.log')
    Check 'ready environment performs no installer downloads or installations' `
        ($script:downloads -eq $beforeDownloads -and $script:installerCalls.Count -eq $beforeInstallers)
    Check 'all shells use repository and UCRT64 environment' `
        (@($script:steps | Where-Object { $_.Directory -ne $repo }).Count -eq 0 -and $env:MSYSTEM -eq 'UCRT64')
    $script:steps = @()
    Invoke-WindowsBuild -Repository $repo -Full -Jobs 2
    Check 'full rebuild also works offline without updating packages' `
        ($script:steps.Count -eq 2 -and $script:steps[-1].Command -like '*--full --installer -j2*')
    $script:networkFailure = $false
    $script:steps = @()
    $script:coreRestart = $true
    Invoke-WindowsBuild -Repository $repo -Full -UpdateTools -Jobs 2
    Check 'runtime restart is followed by mandatory second upgrade and full build' `
        ($script:steps.Count -eq 5 -and $script:steps[2].Name -eq 'Completing MSYS2 system update' -and
        $script:steps[-1].Command -like '*--full --installer -j2*')
    $script:coreRestart = $false
    $script:networkFailure = $true
    $script:steps = @()
    Invoke-WindowsBuild -Repository $repo -GuiOnly -Jobs 5
    Check 'GUI build works offline with the existing GUI build script' `
        ($script:steps.Count -eq 2 -and $script:steps[-1].Command -eq 'cd tools/chatgui && bash build.sh --installer -j5 2>&1 | tee build_chatgui.log')
    $script:steps = @()
    Invoke-WindowsBuild -Repository $repo -PrepareOnly
    Check 'prepare-only checks a ready environment offline without building' ($script:steps.Count -eq 1)
    $script:networkFailure = $false
    $script:steps = @()
    $script:dependencyStatus = 3
    $script:networkFailure = $true
    Expect-Failure 'missing packages do not start updates during an ordinary offline build' {
        Invoke-WindowsBuild -Repository $repo
    } 'automatic package updates are disabled'
    Check 'missing packages stop before any network operation' ($script:steps.Count -eq 1)
    $script:networkFailure = $false
    $script:steps = @()
    Invoke-WindowsBuild -Repository $repo -PrepareOnly
    Check 'explicit preparation installs missing packages without building' `
        ($script:steps.Count -eq 4 -and $script:steps[1].Name -eq 'Updating MSYS2 core packages' -and
        $script:steps[3].Command -eq 'bash tools/chatgui/install_deps_win.sh')
    $script:steps = @()
    $env:MSYS2_ROOT = Join-Path $testRoot 'new-msys64'
    $beforeInstallers = $script:installerCalls.Count
    Invoke-WindowsBuild -Repository $repo
    Check 'clean Windows installation initializes and updates MSYS2 before installing packages' `
        ($script:installerCalls.Count -eq $beforeInstallers + 1 -and $script:steps.Count -eq 6 -and
        $script:steps[0].Name -eq 'Initializing MSYS2 login environment')
    $env:MSYS2_ROOT = $msys
    $script:dependencyStatus = 1
    $script:steps = @()
    Expect-Failure 'package database check errors do not trigger update or build' {
        Invoke-WindowsBuild -Repository $repo
    } 'Local dependency check failed'
    Check 'package query failure stops after the local check' ($script:steps.Count -eq 1)
    $script:dependencyStatus = 0
    foreach ($failure in @('Completing MSYS2 system update', 'Installing UCRT64 build dependencies')) {
        $script:steps = @()
        $script:failStep = $failure
        Expect-Failure "$failure failure stops before build" { Invoke-WindowsBuild -Repository $repo -UpdateTools } 'Injected'
        Check 'no build runs after setup error' (@($script:steps | Where-Object { $_.Name -like 'Building*' }).Count -eq 0)
        Check 'setup failure is recorded before transcript closes' `
            ([IO.File]::ReadAllText((Join-Path $repo 'build_windows_setup.log')).Contains("[ERROR] Injected $failure failure"))
    }
    $script:failStep = 'Building uTun and VibeChat installer'
    Expect-Failure 'build failure is propagated to launcher' { Invoke-WindowsBuild -Repository $repo } 'Injected'
    $script:failStep = ''
    $env:MSYS2_ROOT = Join-Path $testRoot 'unsupported path'
    Expect-Failure 'MSYS2 paths with spaces fail before installation' { Invoke-WindowsBuild -Repository $repo } 'without spaces'
    $env:MSYS2_ROOT = $msys
    $env:PROCESSOR_ARCHITECTURE = 'ARM64'
    Expect-Failure 'unsupported architecture fails before installation' { Invoke-WindowsBuild -Repository $repo } 'architecture=ARM64'
    Write-Host "PASS: $script:checks Windows bootstrap checks; fixture: $testRoot"
} finally {
    foreach ($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name, $savedEnvironment[$name]) }
}
