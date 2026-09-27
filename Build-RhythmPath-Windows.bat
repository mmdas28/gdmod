@echo off
setlocal
set "RPB_SELF=%~f0"
title Rhythm Path - build from source
set "RPB_PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%RPB_PS%" set "RPB_PS=powershell.exe"
"%RPB_PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -STA -Command "$t=[IO.File]::ReadAllText($env:RPB_SELF);$a=$t.IndexOf('#RPB'+'UILD#');if($a -lt 0){Write-Host 'This file is damaged. Download it again.' -ForegroundColor Red;exit 1};& ([ScriptBlock]::Create($t.Substring($a)))"
set "RPB_RC=%ERRORLEVEL%"
echo.
if not "%RPB_RC%"=="0" echo The build stopped with code %RPB_RC%. The full log is in build-log.txt next to this file.
pause
exit /b %RPB_RC%
#RPBUILD#
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
try { $Host.UI.RawUI.WindowTitle = 'Rhythm Path - build from source' } catch { }
Add-Type -AssemblyName System.IO.Compression.FileSystem

$Self = $env:RPB_SELF
$SourceDir = Split-Path -Parent $Self
$ModId = 'mmdas28.rhythm-path'
$UserAgent = 'RhythmPath-Build'
$BindingsCommit = '7f6c2a75742856de88dad354e576dcff8a28e881'
$GitUrl = 'https://github.com/git-for-windows/git/releases/download/v2.52.0.windows.1/MinGit-2.52.0-64-bit.zip'
$CMakeUrl = 'https://github.com/Kitware/CMake/releases/download/v3.31.8/cmake-3.31.8-windows-x86_64.zip'
$NinjaUrl = 'https://github.com/ninja-build/ninja/releases/download/v1.12.1/ninja-win.zip'
$GeodeCliUrl = 'https://github.com/geode-sdk/cli/releases/download/v3.9.0/geode-cli-v3.9.0-win.zip'
$InstallerName = 'RhythmPath-Installer-Windows.bat'
$Log = Join-Path $SourceDir 'build-log.txt'
$script:LogWriter = $null

function Write-Log([string]$Text) {
    if ($script:LogWriter) { try { $script:LogWriter.WriteLine($Text) } catch { } }
}
function Write-Step([string]$Text) { Write-Host ''; Write-Host "==> $Text" -ForegroundColor Cyan; Write-Log "==> $Text" }
function Write-Ok([string]$Text) { Write-Host "    $Text" -ForegroundColor Green; Write-Log "    $Text" }
function Write-Info([string]$Text) { Write-Host "    $Text"; Write-Log "    $Text" }
function Write-Caution([string]$Text) { Write-Host "    $Text" -ForegroundColor Yellow; Write-Log "    $Text" }

function Stop-Build([string]$Message) {
    $e = [Exception]::new($Message)
    $e.Data['rpb'] = $true
    throw $e
}

function Test-File([string]$Path) { return ($Path -and [IO.File]::Exists($Path)) }
function Test-Dir([string]$Path) { return ($Path -and [IO.Directory]::Exists($Path)) }

function Confirm-Choice([string]$Question, [bool]$Default = $true) {
    $suffix = '[y/N]'
    if ($Default) { $suffix = '[Y/n]' }
    while ($true) {
        $a = Read-Host "    $Question $suffix"
        if ($null -eq $a) { $a = '' }
        $a = ([string]$a).Trim().ToLowerInvariant()
        if ($a -eq '') { return $Default }
        if ($a -eq 'y' -or $a -eq 'yes') { return $true }
        if ($a -eq 'n' -or $a -eq 'no') { return $false }
    }
}

function Invoke-Tool([string]$Exe, [string[]]$ArgList, [string]$What) {
    Write-Log "> $Exe $($ArgList -join ' ')"
    $old = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $code = 0
    try {
        & $Exe @ArgList 2>&1 | ForEach-Object {
            $line = "$_"
            Write-Log $line
            Write-Host $line
        }
        $code = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $old
    }
    if ($code -ne 0) { Stop-Build "$What failed (exit code $code). The full output is in build-log.txt." }
}

function Add-ToPath([string]$Dir) {
    $parts = $env:Path -split ';'
    if ($parts -notcontains $Dir) { $env:Path = "$Dir;$env:Path" }
}

function Update-PathFromRegistry {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$env:Path;$machine;$user"
}

function Save-Url([string]$Url, [string]$OutFile) {
    Write-Info "Downloading $Url"
    $part = "$OutFile.part"
    if (Test-File $part) { Remove-Item -LiteralPath $part -Force }
    Invoke-WebRequest -Uri $Url -UseBasicParsing -OutFile $part -Headers @{ 'User-Agent' = $UserAgent }
    Move-Item -LiteralPath $part -Destination $OutFile -Force
}

function Expand-ZipTo([string]$Zip, [string]$Dest) {
    if (Test-Dir $Dest) { Remove-Item -LiteralPath $Dest -Recurse -Force }
    [IO.Compression.ZipFile]::ExtractToDirectory($Zip, $Dest)
}

function Get-PortableTool([string]$Dir, [string]$ExeName, [string]$Url, [string]$Label) {
    $existing = Get-ChildItem -LiteralPath $Dir -Filter $ExeName -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($existing) { return $existing.FullName }
    Write-Info "Getting $Label"
    $zip = "$Dir.zip"
    Save-Url $Url $zip
    Expand-ZipTo $zip $Dir
    Remove-Item -LiteralPath $zip -Force
    $found = Get-ChildItem -LiteralPath $Dir -Filter $ExeName -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $found) { Stop-Build "$Label was downloaded, but $ExeName was not found in it." }
    return $found.FullName
}

function Read-ModJson {
    $path = Join-Path $SourceDir 'mod.json'
    $ok = (Test-File $path) -and (Test-Dir (Join-Path $SourceDir 'src')) -and (Test-File (Join-Path $SourceDir 'installer\windows-template.bat'))
    if (-not $ok) {
        Stop-Build 'Run this file from the extracted Rhythm Path source folder (the one with mod.json, src and installer in it). If you opened the ZIP directly, extract the whole ZIP first.'
    }
    $json = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    if ([string]$json.id -ne $ModId) { Stop-Build "The mod.json in $SourceDir is not Rhythm Path's." }
    return $json
}

function Get-WorkRoot {
    $base = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'RhythmPathBuild'
    if ($base -match '^[A-Za-z]:\\[A-Za-z0-9_.\\-]+$') { return $base }
    return (Join-Path $env:SystemDrive 'RhythmPathBuild')
}

function Find-VisualStudio {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-File $vswhere)) { return $null }
    $found = & $vswhere -latest -products * -version '[17.0,' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
    foreach ($line in @($found)) {
        $path = ([string]$line).Trim()
        if ($path -and (Test-File (Join-Path $path 'VC\Auxiliary\Build\vcvars64.bat'))) { return $path }
    }
    return $null
}

function Find-ClangDir([string]$Vs) {
    $candidates = @()
    if ($Vs) { $candidates += (Join-Path $Vs 'VC\Tools\Llvm\x64\bin') }
    $candidates += (Join-Path $env:ProgramFiles 'LLVM\bin')
    foreach ($dir in $candidates) {
        if ((Test-File (Join-Path $dir 'clang.exe')) -and (Test-File (Join-Path $dir 'clang++.exe'))) { return $dir }
    }
    return $null
}

function Find-Winget {
    $cmd = Get-Command winget.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Install-WithWinget([string]$Id, [string[]]$Extra) {
    $winget = Find-Winget
    if (-not $winget) { return $false }
    $argList = @('install', '--id', $Id, '-e', '--source', 'winget') + $Extra
    Write-Log "> winget $($argList -join ' ')"
    & $winget @argList
    Write-Log "winget exited with code $LASTEXITCODE"
    Update-PathFromRegistry
    return $true
}

function Import-VsEnvironment([string]$Vs) {
    $vcvars = Join-Path $Vs 'VC\Auxiliary\Build\vcvars64.bat'
    $lines = & cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
    if ($LASTEXITCODE -ne 0) { Stop-Build "Could not set up the Visual Studio build environment ($vcvars)." }
    foreach ($line in $lines) {
        $text = [string]$line
        $i = $text.IndexOf('=')
        if ($i -gt 0) { [Environment]::SetEnvironmentVariable($text.Substring(0, $i), $text.Substring($i + 1), 'Process') }
    }
}

function Get-SafeField([string]$Value) {
    if (-not $Value) { return '' }
    return ($Value -replace '[^A-Za-z0-9._+-]', '')
}

function Read-GeodeMeta([string]$GeodeFile) {
    $text = $null
    try {
        $zip = [IO.Compression.ZipFile]::OpenRead($GeodeFile)
        try {
            $entry = $zip.GetEntry('mod.json')
            if ($entry) {
                $reader = New-Object IO.StreamReader($entry.Open())
                try { $text = $reader.ReadToEnd() } finally { $reader.Dispose() }
            }
        }
        finally { $zip.Dispose() }
    }
    catch { $text = $null }
    if (-not $text) { $text = [IO.File]::ReadAllText((Join-Path $SourceDir 'mod.json')) }
    $json = $text | ConvertFrom-Json
    $id = [string]$json.id
    if ($id -notmatch '^[a-z0-9_-]+\.[a-z0-9_.-]+$') { $id = $ModId }
    return @{ Id = $id; Version = (Get-SafeField ([string]$json.version)); Geode = (Get-SafeField ([string]$json.geode)) }
}

function Get-Sha256([byte[]]$Bytes) {
    $sha = [Security.Cryptography.SHA256]::Create()
    try { return (([BitConverter]::ToString($sha.ComputeHash($Bytes))) -replace '-', '').ToLowerInvariant() }
    finally { $sha.Dispose() }
}

function New-Installer([string]$GeodeFile, [string]$Template, [string]$OutFile) {
    $templateText = [IO.File]::ReadAllText($Template)
    if ($templateText -match '[^\x09\x0A\x0D\x20-\x7E]') { Stop-Build 'The installer template must be plain ASCII.' }
    $lines = [Collections.Generic.List[string]]::new()
    foreach ($l in ($templateText -split "`n")) { $lines.Add($l.TrimEnd("`r")) }
    if ($lines.Count -gt 0 -and $lines[$lines.Count - 1] -eq '') { $lines.RemoveAt($lines.Count - 1) }
    if ($lines.Count -eq 0 -or $lines[0] -ne '@echo off') { Stop-Build 'The installer template is damaged (it must start with @echo off).' }
    $markers = 0
    foreach ($l in $lines) {
        if ($l -eq '#RPDATA#' -or $l -eq '#RPEND#') { Stop-Build 'The installer template is damaged (it contains a payload marker).' }
        if ($l -eq '#RPPS#') { $markers++ }
    }
    if ($markers -ne 1) { Stop-Build 'The installer template is damaged (#RPPS# marker).' }

    $meta = Read-GeodeMeta $GeodeFile
    $bytes = [IO.File]::ReadAllBytes($GeodeFile)
    if ($bytes.Length -eq 0) { Stop-Build "The built mod file is empty: $GeodeFile" }
    $sha = Get-Sha256 $bytes
    $b64 = [Convert]::ToBase64String($bytes)

    $sb = New-Object Text.StringBuilder
    foreach ($l in $lines) { [void]$sb.Append($l).Append("`r`n") }
    [void]$sb.Append("#RPDATA#`r`n")
    [void]$sb.Append("#id=$($meta.Id)`r`n")
    [void]$sb.Append("#version=$($meta.Version)`r`n")
    [void]$sb.Append("#geode=$($meta.Geode)`r`n")
    [void]$sb.Append("#size=$($bytes.Length)`r`n")
    [void]$sb.Append("#sha256=$sha`r`n")
    for ($i = 0; $i -lt $b64.Length; $i += 76) {
        [void]$sb.Append($b64.Substring($i, [Math]::Min(76, $b64.Length - $i))).Append("`r`n")
    }
    [void]$sb.Append("#RPEND#`r`n")
    $content = $sb.ToString()

    $check = New-Object Text.StringBuilder
    $inData = $false
    foreach ($l in ($content -split "`r`n")) {
        if ($l -eq '#RPDATA#') { $inData = $true; continue }
        if ($l -eq '#RPEND#') { break }
        if ($inData -and -not $l.StartsWith('#')) { [void]$check.Append($l) }
    }
    if ((Get-Sha256 ([Convert]::FromBase64String($check.ToString()))) -ne $sha) { Stop-Build 'Self-check failed: the installer payload does not match the built mod.' }

    $dir = Split-Path -Parent $OutFile
    if (-not (Test-Dir $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    [IO.File]::WriteAllText($OutFile, $content, [Text.Encoding]::ASCII)
    return $meta
}

function Initialize-Sdk([string]$Root, [string]$Version, [string]$Git, [string]$Geode) {
    $sdk = Join-Path $Root 'geode-sdk'
    $versionFile = Join-Path $sdk 'VERSION'
    $have = ''
    if (Test-File $versionFile) { $have = ([IO.File]::ReadAllText($versionFile)).Trim() }
    if ($have -ne $Version) {
        if (Test-Dir $sdk) { Remove-Item -LiteralPath $sdk -Recurse -Force }
        Invoke-Tool $Git @('clone', '--depth', '1', '--branch', "v$Version", 'https://github.com/geode-sdk/geode.git', $sdk) 'Downloading the Geode SDK'
    }
    else {
        Write-Ok "Geode SDK $Version is already downloaded."
    }
    $env:GEODE_SDK = $sdk
    if (-not (Test-Dir (Join-Path $sdk "bin\$Version"))) {
        Invoke-Tool $Geode @('sdk', 'install-binaries', '--platform', 'win', '--version', $Version) 'Downloading the Geode loader libraries'
    }

    $bindings = Join-Path $Root 'bindings'
    $head = ''
    if (Test-Dir (Join-Path $bindings '.git')) {
        $old = $ErrorActionPreference
        $ErrorActionPreference = 'Continue'
        $head = ([string](& $Git -C $bindings rev-parse HEAD 2>$null)).Trim()
        $ErrorActionPreference = $old
    }
    if ($head -ne $BindingsCommit) {
        if (Test-Dir $bindings) { Remove-Item -LiteralPath $bindings -Recurse -Force }
        New-Item -ItemType Directory -Force -Path $bindings | Out-Null
        Invoke-Tool $Git @('-C', $bindings, 'init', '-q') 'Preparing the game bindings'
        Invoke-Tool $Git @('-C', $bindings, 'remote', 'add', 'origin', 'https://github.com/geode-sdk/bindings.git') 'Preparing the game bindings'
        Invoke-Tool $Git @('-C', $bindings, 'fetch', '--depth', '1', 'origin', $BindingsCommit) 'Downloading the game bindings'
        Invoke-Tool $Git @('-C', $bindings, 'checkout', '-q', 'FETCH_HEAD') 'Downloading the game bindings'
    }
    else {
        Write-Ok 'Game bindings are already downloaded.'
    }
    $env:GEODE_BINDINGS_REPO_PATH = $bindings
}

$code = 0
try {
    try {
        $script:LogWriter = New-Object IO.StreamWriter($Log, $false, [Text.Encoding]::UTF8)
        $script:LogWriter.AutoFlush = $true
    }
    catch { $script:LogWriter = $null }
    Write-Log "Rhythm Path build log, $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"

    Write-Host ''
    Write-Host '  Rhythm Path - build from source' -ForegroundColor White
    Write-Host '  This compiles the mod on this PC and then runs the normal Rhythm Path installer.'

    $mod = Read-ModJson
    $modVersion = [string]$mod.version
    $geodeVersion = ([string]$mod.geode).TrimStart('v')
    if ($geodeVersion -notmatch '^\d+\.\d+\.\d+$') { Stop-Build "mod.json has an unexpected Geode version: '$($mod.geode)'." }
    $root = Get-WorkRoot
    New-Item -ItemType Directory -Force -Path $root | Out-Null
    $tools = Join-Path $root 'tools'
    New-Item -ItemType Directory -Force -Path $tools | Out-Null

    Write-Step 'Checking build tools'
    $vs = Find-VisualStudio
    $clangDir = Find-ClangDir $vs
    $gitCmd = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($vs) { Write-Ok "Visual Studio C++ tools: $vs" } else { Write-Caution 'Visual Studio C++ build tools: not installed' }
    if ($clangDir) { Write-Ok "Clang: $clangDir" } else { Write-Caution 'Clang compiler: not installed' }

    Write-Host ''
    Write-Info "Rhythm Path $modVersion will be built for Geode $geodeVersion in $root."
    if (-not $vs) {
        Write-Info '- Visual Studio 2022 Build Tools with C++ and Clang will be installed (Microsoft, about 4-6 GB).'
        Write-Info '  Windows will ask for administrator rights, and the installer shows Microsoft''s license terms.'
    }
    elseif (-not $clangDir) {
        Write-Info '- LLVM/Clang will be installed (about 500 MB). Windows will ask for administrator rights.'
    }
    $portable = 'CMake, Ninja, the Geode command-line tool'
    if (-not $gitCmd) { $portable = "Git, $portable" }
    Write-Info "- $portable, the Geode SDK and its libraries are downloaded into that folder (about 1-2 GB)."
    Write-Info '- The first build takes roughly 15-40 minutes, later builds a few minutes.'
    if (-not (Confirm-Choice 'Continue?' $true)) {
        Write-Info 'Nothing was changed.'
        exit 0
    }

    if (-not $vs) {
        Write-Step 'Installing Visual Studio 2022 Build Tools'
        $override = '--passive --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.Llvm.Clang'
        $ran = Install-WithWinget 'Microsoft.VisualStudio.2022.BuildTools' @('--override', $override)
        $vs = Find-VisualStudio
        if (-not $vs) {
            Write-Caution 'Visual Studio Build Tools could not be installed automatically.'
            if (-not $ran) { Write-Caution 'The Windows package manager (winget) is not available on this PC.' }
            Write-Caution 'Install them by hand from https://aka.ms/vs/17/release/vs_BuildTools.exe and tick:'
            Write-Caution '  "Desktop development with C++", plus "C++ Clang tools for Windows" under Individual components.'
            Stop-Build 'Then run this file again.'
        }
        Write-Ok "Installed: $vs"
        $clangDir = Find-ClangDir $vs
    }
    if (-not $clangDir) {
        Write-Step 'Installing LLVM/Clang'
        $ran = Install-WithWinget 'LLVM.LLVM' @()
        $clangDir = Find-ClangDir $vs
        if (-not $clangDir) {
            Write-Caution 'Clang could not be installed automatically.'
            Write-Caution 'Open the Visual Studio Installer, choose Modify, and tick "C++ Clang tools for Windows" under Individual components,'
            Write-Caution 'or install LLVM from https://github.com/llvm/llvm-project/releases.'
            Stop-Build 'Then run this file again.'
        }
        Write-Ok "Installed: $clangDir"
    }

    Write-Step 'Setting up the Visual Studio build environment'
    Import-VsEnvironment $vs
    Write-Ok 'Done.'

    Write-Step 'Getting the portable tools'
    if ($gitCmd) {
        $git = $gitCmd.Source
        Write-Ok "Git: $git"
    }
    else {
        $gitDir = Join-Path $tools 'mingit'
        $git = Join-Path $gitDir 'cmd\git.exe'
        if (-not (Test-File $git)) {
            $null = Get-PortableTool $gitDir 'git.exe' $GitUrl 'Git'
            if (-not (Test-File $git)) { Stop-Build "Git was downloaded, but $git was not found in it." }
        }
        Add-ToPath (Split-Path -Parent $git)
        Write-Ok "Git: $git"
    }
    $env:GIT_CONFIG_COUNT = '1'
    $env:GIT_CONFIG_KEY_0 = 'core.longpaths'
    $env:GIT_CONFIG_VALUE_0 = 'true'
    $cmakeDir = Join-Path $tools 'cmake'
    $cmake = Get-ChildItem -LiteralPath $cmakeDir -Filter 'cmake.exe' -Recurse -ErrorAction SilentlyContinue | Where-Object { $_.DirectoryName -like '*\bin' } | Select-Object -First 1
    if ($cmake) { $cmake = $cmake.FullName }
    else { $cmake = Get-PortableTool $cmakeDir 'cmake.exe' $CMakeUrl 'CMake' }
    Add-ToPath (Split-Path -Parent $cmake)
    Write-Ok "CMake: $cmake"
    $ninja = Get-PortableTool (Join-Path $tools 'ninja') 'ninja.exe' $NinjaUrl 'Ninja'
    Add-ToPath (Split-Path -Parent $ninja)
    Write-Ok "Ninja: $ninja"
    $geode = Get-PortableTool (Join-Path $tools 'geode-cli') 'geode.exe' $GeodeCliUrl 'the Geode command-line tool'
    Add-ToPath (Split-Path -Parent $geode)
    Write-Ok "Geode CLI: $geode"

    Write-Step "Getting the Geode SDK $geodeVersion"
    Initialize-Sdk $root $geodeVersion $git $geode

    Write-Step 'Copying the source'
    $modDir = Join-Path $root 'mod'
    & robocopy.exe $SourceDir $modDir /MIR /XD build dist .git .github /XF build-log.txt /NFL /NDL /NJH /NJS /NP /R:1 /W:1 | Out-Null
    if ($LASTEXITCODE -ge 8) { Stop-Build "Copying the source to $modDir failed (robocopy code $LASTEXITCODE)." }
    Write-Ok $modDir

    Write-Step 'Building the mod'
    $buildDir = Join-Path $modDir 'build'
    $clang = (Join-Path $clangDir 'clang.exe') -replace '\\', '/'
    $clangxx = (Join-Path $clangDir 'clang++.exe') -replace '\\', '/'
    $configure = @(
        '-S', $modDir, '-B', $buildDir, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release',
        "-DCMAKE_C_COMPILER=$clang",
        "-DCMAKE_CXX_COMPILER=$clangxx",
        "-DCMAKE_MAKE_PROGRAM=$($ninja -replace '\\', '/')",
        "-DGEODE_CODEGEN_CMAKE_ARGS=-DCMAKE_C_COMPILER=$clang;-DCMAKE_CXX_COMPILER=$clangxx;-G Ninja",
        '-DGEODE_DONT_INSTALL_MODS=ON',
        '-DGEODE_TARGET_PLATFORM=Win64',
        "-DCPM_SOURCE_CACHE=$((Join-Path $root 'cpm-cache') -replace '\\', '/')"
    )
    try {
        Invoke-Tool $cmake $configure 'Configuring the build'
    }
    catch {
        if (-not (Test-Dir $buildDir)) { throw }
        Write-Caution 'Configuring failed with the previous build folder, retrying from a clean one.'
        Remove-Item -LiteralPath $buildDir -Recurse -Force
        Invoke-Tool $cmake $configure 'Configuring the build'
    }
    Invoke-Tool $cmake @('--build', $buildDir, '--config', 'Release', '--parallel') 'Compiling the mod'
    $built = Get-ChildItem -LiteralPath $buildDir -Filter '*.geode' -Recurse -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if (-not $built) { Stop-Build 'The build finished but no .geode file was produced.' }
    Write-Ok "Built: $($built.FullName)"

    Write-Step 'Creating the installer'
    $dist = Join-Path $SourceDir 'dist'
    New-Item -ItemType Directory -Force -Path $dist | Out-Null
    $geodeOut = Join-Path $dist "$ModId.geode"
    Copy-Item -LiteralPath $built.FullName -Destination $geodeOut -Force
    $installer = Join-Path $dist $InstallerName
    $meta = New-Installer $geodeOut (Join-Path $SourceDir 'installer\windows-template.bat') $installer
    Write-Ok "Mod file:  $geodeOut"
    Write-Ok "Installer: $installer"

    Write-Host ''
    Write-Host "  Rhythm Path $($meta.Version) is built." -ForegroundColor Green
    if (Confirm-Choice 'Install it into Geometry Dash now (runs the normal installer)?' $true) {
        Write-Log "> $installer"
        & $installer
        if ($LASTEXITCODE -ne 0 -and $LASTEXITCODE -ne 11) {
            Write-Caution "The installer reported a problem (see above). The built files are in $dist."
            Write-Log "installer exited with code $LASTEXITCODE"
        }
    }
    else {
        Write-Info "You can run $InstallerName from the dist folder later, or copy $ModId.geode into geode\mods yourself."
    }
}
catch {
    $err = $_
    $code = 1
    Write-Host ''
    Write-Host '  The build stopped.' -ForegroundColor Red
    Write-Host "  $($err.Exception.Message)" -ForegroundColor Red
    Write-Log "ERROR: $($err.Exception.Message)"
    if (-not $err.Exception.Data['rpb'] -and $err.InvocationInfo -and $err.InvocationInfo.ScriptLineNumber) {
        Write-Host "  (line $($err.InvocationInfo.ScriptLineNumber))" -ForegroundColor DarkGray
        Write-Log "line $($err.InvocationInfo.ScriptLineNumber)"
    }
}
finally {
    if ($script:LogWriter) { try { $script:LogWriter.Dispose() } catch { } }
}
exit $code
