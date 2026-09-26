@echo off
setlocal
set "RP_SELF=%~f0"
set "RP_GD=%~1"
set "RP_ELEVATED="
set "RP_SID="
title Rhythm Path installer
set "RP_PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%RP_PS%" set "RP_PS=powershell.exe"
"%RP_PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -STA -Command "$t=[IO.File]::ReadAllText($env:RP_SELF);$a=$t.IndexOf('#RP'+'PS#');$b=$t.IndexOf('#RP'+'DATA#');if($a -lt 0 -or $b -lt $a){Write-Host 'This installer file is damaged. Download it again.' -ForegroundColor Red;exit 1};& ([ScriptBlock]::Create($t.Substring($a,$b-$a)))"
set "RP_RC=%ERRORLEVEL%"
if "%RP_RC%"=="0" exit /b 0
if "%RP_RC%"=="10" exit /b 10
if "%RP_RC%"=="11" exit /b 0
echo.
echo The installer stopped unexpectedly (code %RP_RC%).
echo You can still install the mod by hand: copy mmdas28.rhythm-path.geode from the release
echo into the "geode\mods" folder inside your Geometry Dash folder.
echo.
pause
exit /b %RP_RC%
#RPPS#
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch { }
try { $Host.UI.RawUI.WindowTitle = 'Rhythm Path installer' } catch { }

$Self = $env:RP_SELF
$SteamAppId = '322170'
$DefaultModId = 'mmdas28.rhythm-path'
$DefaultGeode = '5.10.1'
$UserAgent = 'RhythmPath-Installer'
$State = @{ Code = 0; TempDir = $null }

function Write-Step([string]$Text) { Write-Host ''; Write-Host "==> $Text" -ForegroundColor Cyan }
function Write-Ok([string]$Text) { Write-Host "    $Text" -ForegroundColor Green }
function Write-Info([string]$Text) { Write-Host "    $Text" }
function Write-Caution([string]$Text) { Write-Host "    $Text" -ForegroundColor Yellow }

function Stop-Install([string]$Message) {
    $e = [Exception]::new($Message)
    $e.Data['rp'] = $true
    throw $e
}

function Test-File([string]$Path) { return ($Path -and [IO.File]::Exists($Path)) }
function Test-Dir([string]$Path) { return ($Path -and [IO.Directory]::Exists($Path)) }

function Read-Answer([string]$Prompt) {
    $a = Read-Host "    $Prompt"
    if ($null -eq $a) { return '' }
    return ([string]$a).Trim()
}

function Confirm-Choice([string]$Question, [bool]$Default = $true) {
    $suffix = '[y/N]'
    if ($Default) { $suffix = '[Y/n]' }
    while ($true) {
        $a = Read-Answer "$Question $suffix"
        if ($a -eq '') { return $Default }
        if ($a -match '^(y|yes)$') { return $true }
        if ($a -match '^(n|no)$') { return $false }
    }
}

function Get-TempDir {
    if (-not $State.TempDir) {
        $State.TempDir = Join-Path ([IO.Path]::GetTempPath()) ('RhythmPathInstaller-' + [guid]::NewGuid().ToString('N'))
        [void][IO.Directory]::CreateDirectory($State.TempDir)
    }
    return $State.TempDir
}

function Get-BytesSha256([byte[]]$Bytes) {
    $h = [Security.Cryptography.SHA256]::Create()
    try { return ([BitConverter]::ToString($h.ComputeHash($Bytes)) -replace '-', '').ToLowerInvariant() } finally { $h.Dispose() }
}

function Get-FileSha256([string]$Path) {
    $h = [Security.Cryptography.SHA256]::Create()
    $s = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($h.ComputeHash($s)) -replace '-', '').ToLowerInvariant() } finally { $s.Dispose(); $h.Dispose() }
}

function Test-AccessDenied($ErrorRecord) {
    $e = $ErrorRecord
    if ($ErrorRecord -is [Management.Automation.ErrorRecord]) { $e = $ErrorRecord.Exception }
    while ($e) {
        if ($e -is [UnauthorizedAccessException]) { return $true }
        if ($e -is [Security.SecurityException]) { return $true }
        if ($e -is [IO.IOException] -and (($e.HResult -band 0xFFFF) -eq 5)) { return $true }
        $e = $e.InnerException
    }
    return $false
}

function Read-Payload {
    if (-not (Test-File $Self)) { Stop-Install 'Could not read the installer file. Start it by double-clicking RhythmPath-Installer-Windows.bat.' }
    $text = [IO.File]::ReadAllText($Self)
    $begin = $text.IndexOf('#RP' + 'DATA#', [StringComparison]::Ordinal)
    $end = $text.IndexOf('#RP' + 'END#', [StringComparison]::Ordinal)
    if ($begin -lt 0 -or $end -lt $begin) { Stop-Install 'This installer file is incomplete. Download it again from the Releases page.' }
    $meta = @{}
    $b64 = New-Object Text.StringBuilder
    foreach ($line in ($text.Substring($begin, $end - $begin) -split "`n")) {
        $l = $line.Trim()
        if ($l.StartsWith('#')) {
            if ($l -match '^#([a-z0-9]+)=(.*)$') { $meta[$Matches[1]] = $Matches[2].Trim() }
        } elseif ($l) {
            [void]$b64.Append($l)
        }
    }
    try { $bytes = [Convert]::FromBase64String($b64.ToString()) } catch { Stop-Install 'The mod inside this installer is damaged. Download the installer again.' }
    $sha = Get-BytesSha256 $bytes
    if ($bytes.Length -eq 0 -or -not $meta['sha256'] -or $sha -ne $meta['sha256'].ToLowerInvariant()) {
        Stop-Install 'The mod inside this installer is damaged (checksum mismatch). Download the installer again.'
    }
    $id = $DefaultModId
    if ($meta['id'] -match '^[a-z0-9_\-]+\.[a-z0-9_\-.]+$') { $id = $meta['id'] }
    $geodeText = $DefaultGeode
    if ($meta['geode'] -match '(\d+)\.(\d+)\.(\d+)') { $geodeText = $Matches[0] }
    $geodeParts = $geodeText.Split('.')
    return @{
        Bytes    = $bytes
        Sha256   = $sha
        Id       = $id
        Version  = [string]$meta['version']
        MinGeode = [version]::new([int]$geodeParts[0], [int]$geodeParts[1], [int]$geodeParts[2])
    }
}

function Get-RegValue([string]$Key, [string]$Name) {
    try {
        $v = (Get-ItemProperty -LiteralPath $Key -Name $Name -ErrorAction Stop).$Name
        if ($v) { return [string]$v }
    } catch { }
    return $null
}

function Get-SteamLibraries {
    $roots = New-Object System.Collections.Generic.List[string]
    foreach ($r in @(
            (Get-RegValue 'HKCU:\Software\Valve\Steam' 'SteamPath'),
            (Get-RegValue 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam' 'InstallPath'),
            (Get-RegValue 'HKLM:\SOFTWARE\Valve\Steam' 'InstallPath'))) {
        if ($r) { $roots.Add(($r -replace '/', '\')) }
    }
    foreach ($pf in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if ($pf) { $roots.Add((Join-Path $pf 'Steam')) }
    }
    $libs = New-Object System.Collections.Generic.List[string]
    foreach ($root in $roots) {
        if (-not (Test-Dir $root)) { continue }
        $libs.Add($root)
        foreach ($vdf in @((Join-Path $root 'steamapps\libraryfolders.vdf'), (Join-Path $root 'config\libraryfolders.vdf'))) {
            if (-not (Test-File $vdf)) { continue }
            try { $content = [IO.File]::ReadAllText($vdf) } catch { continue }
            foreach ($m in [regex]::Matches($content, '"(?:path|\d+)"\s+"([^"\r\n]+)"')) {
                $p = $m.Groups[1].Value -replace '\\\\', '\'
                if ($p -match '^(?:[A-Za-z]:\\|\\\\)' -and (Test-Dir $p)) { $libs.Add($p) }
            }
        }
    }
    return $libs.ToArray()
}

function Test-GDFolder([string]$Dir) {
    if (-not (Test-Dir $Dir)) { return $false }
    if (-not (Test-File (Join-Path $Dir 'libcocos2d.dll'))) { return $false }
    return (@(Get-ChildItem -LiteralPath $Dir -Filter '*.exe' -File -ErrorAction SilentlyContinue).Count -gt 0)
}

function Resolve-GDCandidate([string]$Path) {
    if (-not $Path) { return $null }
    $Path = $Path.Trim().Trim('"').Trim()
    if (-not $Path) { return $null }
    try { $full = [IO.Path]::GetFullPath($Path) } catch { return $null }
    if (Test-File $full) { $full = [IO.Path]::GetDirectoryName($full) }
    $tries = New-Object System.Collections.Generic.List[string]
    foreach ($sub in @('', 'Geometry Dash', 'common\Geometry Dash', 'steamapps\common\Geometry Dash')) {
        if ($sub) { $tries.Add([IO.Path]::Combine($full, $sub)) } else { $tries.Add($full) }
    }
    $up = $full
    for ($i = 0; $i -lt 3; $i++) {
        $up = [IO.Path]::GetDirectoryName($up.TrimEnd('\', '/'))
        if (-not $up) { break }
        $tries.Add($up)
    }
    foreach ($t in $tries) {
        if (Test-GDFolder $t) { return [IO.Path]::GetFullPath($t) }
    }
    return $null
}

function Find-GDFolders {
    $found = New-Object System.Collections.Generic.List[string]
    $seen = @{}
    $add = {
        param([string]$Candidate)
        if (-not $Candidate) { return }
        try { $full = [IO.Path]::GetFullPath($Candidate) } catch { return }
        $key = $full.TrimEnd('\', '/').ToLowerInvariant()
        if ($seen.ContainsKey($key)) { return }
        $seen[$key] = $true
        if (Test-GDFolder $full) { $found.Add($full) }
    }
    foreach ($lib in @(Get-SteamLibraries)) {
        & $add (Join-Path $lib 'steamapps\common\Geometry Dash')
    }
    foreach ($k in @(
            "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App $SteamAppId",
            "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App $SteamAppId",
            "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\Steam App $SteamAppId")) {
        & $add (Get-RegValue $k 'InstallLocation')
    }
    if ($found.Count -eq 0) {
        $rels = @(
            'Program Files (x86)\Steam\steamapps\common\Geometry Dash',
            'Program Files\Steam\steamapps\common\Geometry Dash',
            'Steam\steamapps\common\Geometry Dash',
            'SteamLibrary\steamapps\common\Geometry Dash',
            'Games\Steam\steamapps\common\Geometry Dash',
            'Games\SteamLibrary\steamapps\common\Geometry Dash',
            'Games\Geometry Dash',
            'Geometry Dash')
        foreach ($drive in [IO.DriveInfo]::GetDrives()) {
            try {
                if (-not $drive.IsReady) { continue }
                if ($drive.DriveType -ne [IO.DriveType]::Fixed -and $drive.DriveType -ne [IO.DriveType]::Removable) { continue }
                $root = $drive.RootDirectory.FullName
            } catch { continue }
            foreach ($rel in $rels) { & $add (Join-Path $root $rel) }
        }
    }
    return $found.ToArray()
}

function Get-DialogStartFolder {
    foreach ($lib in @(Get-SteamLibraries)) {
        $common = Join-Path $lib 'steamapps\common'
        if (Test-Dir $common) { return $common }
    }
    return $null
}

function Request-GDFolder {
    $dialogOk = $true
    try { Add-Type -AssemblyName System.Windows.Forms } catch { $dialogOk = $false }
    $start = Get-DialogStartFolder
    while ($true) {
        $picked = $null
        if ($dialogOk) {
            Write-Info 'A window will open. Choose the Geometry Dash folder (the one that contains GeometryDash.exe).'
            Write-Info 'Tip: in Steam, right-click Geometry Dash > Manage > Browse local files to see where it is.'
            try {
                $owner = New-Object System.Windows.Forms.Form
                $owner.TopMost = $true
                $owner.ShowInTaskbar = $false
                $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
                $dlg.Description = 'Select the Geometry Dash folder (it contains GeometryDash.exe)'
                $dlg.ShowNewFolderButton = $false
                if ($start) { $dlg.SelectedPath = $start }
                if ($dlg.ShowDialog($owner) -eq [System.Windows.Forms.DialogResult]::OK) { $picked = $dlg.SelectedPath }
                $dlg.Dispose()
                $owner.Dispose()
            } catch {
                $dialogOk = $false
            }
        }
        if (-not $picked) {
            $picked = Read-Answer 'Paste the full path of your Geometry Dash folder (or press Enter to cancel)'
            if (-not $picked) { Stop-Install 'No Geometry Dash folder was chosen, so nothing was installed.' }
        }
        $gd = Resolve-GDCandidate $picked
        if ($gd) { return $gd }
        Write-Caution "That folder does not contain Geometry Dash (GeometryDash.exe and libcocos2d.dll): $picked"
        $start = $picked
    }
}

function Select-GDFolder {
    if ($env:RP_GD) {
        $given = Resolve-GDCandidate $env:RP_GD
        if ($given) { return $given }
        Write-Caution "This is not a Geometry Dash folder: $($env:RP_GD)"
    }
    Write-Step 'Looking for Geometry Dash'
    $found = @(Find-GDFolders)
    if ($found.Count -eq 1) { return $found[0] }
    if ($found.Count -gt 1) {
        Write-Info 'Found more than one Geometry Dash folder:'
        for ($i = 0; $i -lt $found.Count; $i++) { Write-Info ('  [{0}] {1}' -f ($i + 1), $found[$i]) }
        Write-Info '  [B] Browse for a different folder'
        while ($true) {
            $a = Read-Answer 'Which one? (press Enter for 1)'
            if ($a -eq '') { return $found[0] }
            if ($a -match '^[bB]$') { break }
            $n = 0
            if ([int]::TryParse($a, [ref]$n) -and $n -ge 1 -and $n -le $found.Count) { return $found[$n - 1] }
        }
    } else {
        Write-Caution 'Could not find Geometry Dash automatically.'
    }
    return (Request-GDFolder)
}

function Assert-GDVersion([string]$Gd) {
    $missing = @(@('libpng16.dll', 'pthreadVC3.dll', 'libcrypto-3-x64.dll') | Where-Object { -not (Test-File (Join-Path $Gd $_)) })
    if ($missing.Count -eq 0) { return }
    Write-Caution 'This copy of Geometry Dash looks older than version 2.206.'
    Write-Caution 'Geode and Rhythm Path need Geometry Dash 2.2081. Update the game through Steam first.'
    if (-not (Confirm-Choice 'Continue anyway?' $false)) { Stop-Install 'Update Geometry Dash through Steam, then run this installer again.' }
}

function Wait-GDClosed {
    $warned = $false
    while (@(Get-Process -Name 'GeometryDash' -ErrorAction SilentlyContinue).Count -gt 0) {
        if (-not $warned) {
            Write-Host ''
            Write-Caution 'Geometry Dash is running. Please close it completely.'
            Write-Caution '(Its files are locked while it runs, and mods only load when the game starts.)'
            $warned = $true
        }
        $redirected = $false
        try { $redirected = [Console]::IsInputRedirected } catch { }
        if ($redirected) { Stop-Install 'Close Geometry Dash, then run the installer again.' }
        [void](Read-Answer 'Press Enter once Geometry Dash is closed...')
    }
}

function Test-WriteAccess([string]$Dir) {
    try {
        [void][IO.Directory]::CreateDirectory($Dir)
        $probe = Join-Path $Dir ('.rp-write-test-' + [guid]::NewGuid().ToString('N'))
        [IO.File]::WriteAllText($probe, 'ok')
        [IO.File]::Delete($probe)
        return $true
    } catch {
        if (Test-AccessDenied $_) { return $false }
        throw
    }
}

function ConvertTo-PsLiteral([string]$Text) {
    $b64 = [Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes($Text))
    return "[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String('$b64'))"
}

function Test-IsAdmin {
    try {
        return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    } catch {
        return $false
    }
}

function Restart-Elevated([string]$Gd) {
    if ($env:RP_ELEVATED -eq '1' -or (Test-IsAdmin)) {
        Stop-Install ("Windows refused to write to $Gd even with administrator rights. " +
            'Make sure the folder is not read-only and that your antivirus (for example Controlled folder access) is not blocking it.')
    }
    Write-Host ''
    Write-Caution "Windows did not allow writing to: $Gd"
    Write-Info 'This happens when Geometry Dash is in a protected folder such as Program Files.'
    Write-Info 'Windows will now ask for administrator permission. The install then continues in a new window.'
    Write-Info 'That window also gives your Windows account write access to the folder, which Geode needs every time the game starts.'
    $sid = ''
    try { $sid = [string][Security.Principal.WindowsIdentity]::GetCurrent().User.Value } catch { }
    $loader = 'trap{Write-Host $_ -ForegroundColor Red;[void](Read-Host ''Press Enter to close'');exit 1};$t=[IO.File]::ReadAllText($env:RP_SELF);$a=$t.IndexOf(''#RP''+''PS#'');$b=$t.IndexOf(''#RP''+''DATA#'');& ([ScriptBlock]::Create($t.Substring($a,$b-$a)))'
    $boot = '$env:RP_SELF={0};$env:RP_GD={1};$env:RP_SID={2};$env:RP_ELEVATED=''1'';{3}' -f (ConvertTo-PsLiteral $Self), (ConvertTo-PsLiteral $Gd), (ConvertTo-PsLiteral $sid), $loader
    $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($boot))
    $exe = $null
    try { $exe = (Get-Process -Id $PID).Path } catch { }
    if (-not $exe) { $exe = 'powershell.exe' }
    try {
        Start-Process -FilePath $exe -Verb RunAs -ArgumentList @('-NoLogo', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-STA', '-EncodedCommand', $encoded) | Out-Null
    } catch {
        Stop-Install ('Administrator permission was not given, so the installer cannot write to the Geometry Dash folder. ' +
            'Run the installer again and click Yes, or move Geometry Dash to a Steam library outside Program Files.')
    }
    Write-Ok 'Continuing in the administrator window. This window will close.'
    Start-Sleep -Seconds 3
}

function Grant-UserAccess([string]$Gd) {
    $ErrorActionPreference = 'Continue'
    $sid = [string]$env:RP_SID
    if ($sid -notmatch '^S-1-[0-9]+(-[0-9]+)+$') { $sid = 'S-1-5-32-545' }
    $target = $Gd.TrimEnd('\', '/')
    if ($target -match '^[A-Za-z]:$') { $target += '\' }
    $icacls = Join-Path (Get-SystemDir) 'icacls.exe'
    if (-not (Test-File $icacls)) { $icacls = 'icacls.exe' }
    Write-Step 'Giving your Windows account write access to the Geometry Dash folder'
    Write-Info 'Geode writes to this folder every time the game starts. This can take a moment...'
    $code = -1
    $out = ''
    try {
        $out = (& $icacls $target '/grant' ('*{0}:(OI)(CI)M' -f $sid) '/T' '/C' '/Q' 2>&1 | Out-String).Trim()
        $code = $LASTEXITCODE
    } catch {
        $out = $_.Exception.Message
    }
    if ($code -eq 0) {
        Write-Ok 'Folder permissions updated.'
        return $true
    }
    Write-Caution "Windows could not update the permissions of $Gd (icacls exit code $code)."
    if ($out) { foreach ($l in @($out -split "`r?`n" | Select-Object -Last 3)) { Write-Caution $l } }
    return $false
}

function Save-Url([string]$Url, [string]$Path, [string]$Label) {
    $lastError = $null
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            $req = [Net.WebRequest]::Create($Url)
            $req.UserAgent = $UserAgent
            $req.Timeout = 30000
            $req.ReadWriteTimeout = 120000
            $res = $req.GetResponse()
            try {
                $total = $res.ContentLength
                if ($total -le 0) { Write-Info "Downloading $Label..." }
                $in = $res.GetResponseStream()
                $out = [IO.File]::Create($Path)
                try {
                    $buf = New-Object byte[] 262144
                    $done = [long]0
                    $shown = -1
                    while (($n = $in.Read($buf, 0, $buf.Length)) -gt 0) {
                        $out.Write($buf, 0, $n)
                        $done += $n
                        if ($total -gt 0) {
                            $pct = [int][Math]::Floor(100 * $done / $total)
                            if ($pct -ne $shown) {
                                Write-Host -NoNewline ("`r    Downloading {0}: {1,3}% ({2:N1} / {3:N1} MB)" -f $Label, $pct, ($done / 1MB), ($total / 1MB))
                                $shown = $pct
                            }
                        }
                    }
                } finally {
                    $out.Dispose()
                    $in.Dispose()
                }
            } finally {
                $res.Dispose()
            }
            if ($total -gt 0) { Write-Host '' }
            if ($total -gt 0 -and $done -ne $total) { throw "the download was cut off ($done of $total bytes)" }
            if ($done -eq 0) { throw 'the server sent an empty file' }
            return
        } catch {
            Write-Host ''
            $lastError = $_.Exception.Message
            if (Test-File $Path) { Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue }
            if ($attempt -lt 3) {
                Write-Caution "Download failed ($lastError). Retrying..."
                Start-Sleep -Seconds 2
            }
        }
    }
    Stop-Install "Could not download $Label from $Url ($lastError). Check your internet connection and try again."
}

function Assert-FileSha([string]$Path, [string]$Expected, [string]$Label) {
    if (-not $Expected) { return }
    $actual = Get-FileSha256 $Path
    if ($actual -ne $Expected.ToLowerInvariant()) { Stop-Install "The $Label download is damaged (checksum mismatch). Run the installer again." }
}

function Get-AssetSha($Asset) {
    if ($Asset.PSObject.Properties['digest'] -and ([string]$Asset.digest) -match '^sha256:([0-9a-fA-F]{64})$') { return $Matches[1].ToLowerInvariant() }
    return $null
}

function Get-GeodeRelease([version]$Min) {
    $pin = 'v' + $Min.ToString(3)
    $base = 'https://github.com/geode-sdk/geode/releases/download'
    $rel = @{
        Tag       = $pin
        Zip       = "$base/$pin/geode-$pin-win.zip"
        ZipSha    = $null
        Resources = "$base/$pin/resources.zip"
        ResSha    = $null
    }
    Write-Info 'Checking for the newest Geode release...'
    try {
        $r = Invoke-RestMethod -UseBasicParsing -TimeoutSec 20 -UserAgent $UserAgent -Uri 'https://api.github.com/repos/geode-sdk/geode/releases/latest'
        if ([string]$r.tag_name -match '^v(\d+)\.(\d+)\.(\d+)$') {
            $v = [version]::new([int]$Matches[1], [int]$Matches[2], [int]$Matches[3])
            if ($v.Major -eq $Min.Major -and $v -ge $Min) {
                $zip = @($r.assets | Where-Object { $_.name -match '^geode-v[\d.]+-win\.zip$' }) | Select-Object -First 1
                $res = @($r.assets | Where-Object { $_.name -eq 'resources.zip' }) | Select-Object -First 1
                if ($zip) {
                    $rel.Tag = [string]$r.tag_name
                    $rel.Zip = [string]$zip.browser_download_url
                    $rel.ZipSha = Get-AssetSha $zip
                    $rel.Resources = $null
                    if ($res) {
                        $rel.Resources = [string]$res.browser_download_url
                        $rel.ResSha = Get-AssetSha $res
                    }
                }
            }
        }
    } catch {
        Write-Caution "Could not reach GitHub to check for updates ($($_.Exception.Message)). Using Geode $pin."
    }
    return $rel
}

function Expand-ZipFile([string]$Zip, [string]$Dest, [string[]]$Skip, [string[]]$Require) {
    Add-Type -AssemblyName System.IO.Compression
    $fs = [IO.File]::OpenRead($Zip)
    try {
        $archive = New-Object IO.Compression.ZipArchive($fs, [IO.Compression.ZipArchiveMode]::Read)
        try {
            $names = @($archive.Entries | ForEach-Object { $_.FullName })
            foreach ($r in $Require) {
                if ($names -notcontains $r) { Stop-Install "The downloaded Geode package does not contain $r. Run the installer again." }
            }
            $root = [IO.Path]::GetFullPath($Dest).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
            foreach ($entry in $archive.Entries) {
                if ($entry.FullName.EndsWith('/') -or $entry.FullName.EndsWith('\') -or -not $entry.Name) { continue }
                $skipIt = $false
                foreach ($s in $Skip) { if ($entry.Name -like $s) { $skipIt = $true } }
                if ($skipIt) { continue }
                $target = [IO.Path]::GetFullPath((Join-Path $Dest $entry.FullName))
                if (-not $target.StartsWith($root, [StringComparison]::OrdinalIgnoreCase)) { continue }
                [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target))
                $in = $entry.Open()
                try {
                    $out = [IO.File]::Create($target)
                    try { $in.CopyTo($out) } finally { $out.Dispose() }
                } finally {
                    $in.Dispose()
                }
            }
        } finally {
            $archive.Dispose()
        }
    } finally {
        $fs.Dispose()
    }
}

function Get-SystemDir {
    if ([Environment]::Is64BitOperatingSystem -and -not [Environment]::Is64BitProcess) { return (Join-Path $env:WINDIR 'Sysnative') }
    return [Environment]::SystemDirectory
}

function Test-VCRuntime {
    $sys = Get-SystemDir
    if (-not $sys) { return $false }
    foreach ($f in @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')) {
        if (-not (Test-File (Join-Path $sys $f))) { return $false }
    }
    try {
        $vi = (Get-Item -LiteralPath (Join-Path $sys 'vcruntime140.dll')).VersionInfo
        return ($vi.FileMajorPart -gt 14 -or ($vi.FileMajorPart -eq 14 -and $vi.FileMinorPart -ge 44))
    } catch {
        return $false
    }
}

function Install-VCRuntime {
    if (Test-VCRuntime) { return }
    $url = 'https://aka.ms/vc14/vc_redist.x64.exe'
    Write-Caution 'Geode needs a recent Microsoft Visual C++ Runtime (2015-2022, version 14.44 or newer).'
    if (-not (Confirm-Choice 'Download and install it now? Windows will ask for permission.' $true)) {
        Write-Caution "Skipped. If Geometry Dash does not start, install it from $url"
        return
    }
    $exe = Join-Path (Get-TempDir) 'vc_redist.x64.exe'
    Save-Url $url $exe 'Visual C++ Runtime'
    Write-Info 'Installing the Visual C++ Runtime (this can take a minute)...'
    try {
        $p = Start-Process -FilePath $exe -ArgumentList @('/install', '/quiet', '/norestart') -Verb RunAs -Wait -PassThru
        $code = $p.ExitCode
    } catch {
        Write-Caution "The Visual C++ Runtime was not installed ($($_.Exception.Message))."
        Write-Caution "If Geometry Dash does not start, install it from $url"
        return
    }
    if ($code -eq 0 -or $code -eq 1638) { Write-Ok 'Visual C++ Runtime installed.' }
    elseif ($code -eq 3010) { Write-Ok 'Visual C++ Runtime installed. Restart your PC if the game does not start.' }
    else { Write-Caution "The Visual C++ Runtime installer exited with code $code. If Geometry Dash does not start, install it from $url" }
}

function Remove-IfExists([string]$Path) {
    if (Test-File $Path) { Remove-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue }
    elseif (Test-Dir $Path) { Remove-Item -LiteralPath $Path -Recurse -Force -ErrorAction SilentlyContinue }
}

function Get-GeodeStatus([string]$Gd, [version]$Min) {
    $dll = Join-Path $Gd 'Geode.dll'
    $proxy = Join-Path $Gd 'XInput1_4.dll'
    if (-not (Test-File $dll)) {
        if (Test-File $proxy) { return @{ State = 'other'; Version = $null } }
        return @{ State = 'missing'; Version = $null }
    }
    $v = [version]::new(0, 0, 0)
    try {
        $vi = (Get-Item -LiteralPath $dll).VersionInfo
        $v = [version]::new([int]$vi.FileMajorPart, [int]$vi.FileMinorPart, [int]$vi.FileBuildPart)
    } catch { }
    if (-not (Test-File $proxy)) { return @{ State = 'broken'; Version = $v } }
    if ($v.Major -gt $Min.Major) { return @{ State = 'newer'; Version = $v } }
    if ($v -lt $Min) { return @{ State = 'old'; Version = $v } }
    return @{ State = 'ok'; Version = $v }
}

function Install-Geode([string]$Gd, [version]$Min) {
    Write-Step 'Installing Geode (the mod loader)'
    Install-VCRuntime
    $rel = Get-GeodeRelease $Min
    $tmp = Get-TempDir
    $zip = Join-Path $tmp 'geode-win.zip'
    Save-Url $rel.Zip $zip "Geode $($rel.Tag)"
    Assert-FileSha $zip $rel.ZipSha "Geode $($rel.Tag)"
    Wait-GDClosed
    Write-Info 'Extracting Geode...'
    Expand-ZipFile $zip $Gd @('*.lib') @('Geode.dll', 'XInput1_4.dll')
    foreach ($p in @('geode\update', 'geode\index', 'xinput9_1_0.dll', 'xinput9_1_0.lib', 'xinput9_1_0.pdb', 'hackpro.dll')) {
        Remove-IfExists (Join-Path $Gd $p)
    }
    if (Test-VCRuntime) {
        foreach ($p in @('msvcp140.dll', 'msvcp140d.dll', 'vcruntime140.dll', 'vcruntime140d.dll')) { Remove-IfExists (Join-Path $Gd $p) }
    }
    try { [IO.File]::WriteAllText((Join-Path $Gd 'steam_appid.txt'), $SteamAppId) } catch { }
    Write-Ok "Geode $($rel.Tag) installed."
    if ($rel.Resources) {
        try {
            $resZip = Join-Path $tmp 'resources.zip'
            Save-Url $rel.Resources $resZip 'Geode resources'
            Assert-FileSha $resZip $rel.ResSha 'Geode resources'
            Expand-ZipFile $resZip (Join-Path $Gd 'geode\resources\geode.loader') @() @()
            Write-Ok 'Geode resources installed.'
        } catch {
            Write-Caution "Could not install Geode's resources ($($_.Exception.Message))."
            Write-Caution 'That is OK: Geode downloads them itself the first time the game starts.'
        }
    }
}

function Install-ModFile([string]$ModsDir, $Payload) {
    [void][IO.Directory]::CreateDirectory($ModsDir)
    $final = Join-Path $ModsDir ($Payload.Id + '.geode')
    $tmp = $final + '.rp-new'
    $stale = '^' + [regex]::Escape($Payload.Id) + '(?: ?\(\d+\)|-\d+)\.geode$'
    foreach ($f in @(Get-ChildItem -LiteralPath $ModsDir -File -ErrorAction SilentlyContinue)) {
        if ($f.Name -match $stale) {
            Remove-Item -LiteralPath $f.FullName -Force
            Write-Info "Removed an old duplicate: $($f.Name)"
        }
    }
    [IO.File]::WriteAllBytes($tmp, $Payload.Bytes)
    if ((Get-FileSha256 $tmp) -ne $Payload.Sha256) {
        Remove-IfExists $tmp
        Stop-Install 'The mod file could not be written correctly (checksum mismatch). Check your disk and try again.'
    }
    [IO.File]::Copy($tmp, $final, $true)
    Remove-IfExists $tmp
    if ((Get-FileSha256 $final) -ne $Payload.Sha256) { Stop-Install "The mod file at $final does not match the expected checksum. Run the installer again." }
    return $final
}

function Test-SteamProtocol {
    try { return [bool](Test-Path -LiteralPath 'Registry::HKEY_CLASSES_ROOT\steam\shell\open\command') } catch { return $false }
}

function Test-SteamCopy([string]$Gd) {
    if (-not (Test-SteamProtocol)) { return $false }
    try { $key = [IO.Path]::GetFullPath($Gd).TrimEnd('\', '/') } catch { return $false }
    foreach ($lib in @(Get-SteamLibraries)) {
        try { $p = [IO.Path]::GetFullPath((Join-Path $lib 'steamapps\common\Geometry Dash')).TrimEnd('\', '/') } catch { continue }
        if ([string]::Equals($p, $key, [StringComparison]::OrdinalIgnoreCase)) { return $true }
    }
    return $false
}

function Get-GDExe([string]$Gd) {
    $main = Join-Path $Gd 'GeometryDash.exe'
    if (Test-File $main) { return $main }
    $exes = @(Get-ChildItem -LiteralPath $Gd -Filter '*.exe' -File -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike 'GeodeUpdater*' })
    if ($exes.Count -eq 1) { return $exes[0].FullName }
    return $null
}

function Start-GD([string]$Gd) {
    if (Test-SteamCopy $Gd) {
        try {
            Start-Process "steam://rungameid/$SteamAppId"
            Write-Ok 'Starting Geometry Dash through Steam...'
            return
        } catch { }
    }
    $exe = Get-GDExe $Gd
    if ($exe) {
        try {
            $psi = New-Object Diagnostics.ProcessStartInfo -Property @{ FileName = $exe; WorkingDirectory = $Gd; UseShellExecute = $true }
            $proc = [Diagnostics.Process]::Start($psi)
            if ($proc) { $proc.Dispose() }
            Write-Ok 'Starting Geometry Dash...'
            return
        } catch { }
    }
    Write-Caution "Could not start Geometry Dash automatically. Start it the way you usually do (it is in $Gd)."
}

function Invoke-Main {
    Write-Host ''
    Write-Host '  Rhythm Path installer' -ForegroundColor White
    Write-Host '  ---------------------'
    $payload = Read-Payload
    $title = 'Rhythm Path'
    if ($payload.Version) { $title = "Rhythm Path $($payload.Version)" }
    Write-Info "Mod: $title ($($payload.Id))"
    $elevated = ($env:RP_ELEVATED -eq '1') -or (Test-IsAdmin)
    if ($elevated) { Write-Info 'Running with administrator rights.' }

    $gd = Select-GDFolder
    Write-Ok "Geometry Dash folder: $gd"
    Assert-GDVersion $gd

    Write-Step 'Checking Geode'
    $geode = Get-GeodeStatus $gd $payload.MinGeode
    $needGeode = $true
    switch ($geode.State) {
        'ok' {
            Write-Ok "Geode v$($geode.Version) is installed."
            $needGeode = $false
        }
        'newer' {
            Write-Caution "Geode v$($geode.Version) is installed, but this build of Rhythm Path was made for Geode v$($payload.MinGeode)."
            Write-Caution 'It may not load until a matching Rhythm Path update is released.'
            $needGeode = $false
        }
        'old' { Write-Info "Geode v$($geode.Version) is installed, but Rhythm Path needs v$($payload.MinGeode) or newer. Geode will be updated." }
        'broken' { Write-Info 'Geode is only partly installed. It will be reinstalled.' }
        'other' {
            Write-Caution 'A different mod loader (XInput1_4.dll without Geode) is installed in this folder.'
            if (-not (Confirm-Choice 'Replace it with Geode?' $true)) { Stop-Install 'Cancelled. Nothing was changed.' }
        }
        default { Write-Info 'Geode (the mod loader) is not installed yet. It will be installed now.' }
    }

    Wait-GDClosed

    $modsDir = Join-Path $gd 'geode\mods'
    $checkDirs = @($modsDir)
    if ($needGeode) { $checkDirs = @($gd, $modsDir) }
    foreach ($dir in $checkDirs) {
        if (-not (Test-WriteAccess $dir)) {
            Restart-Elevated $gd
            $State.Code = 11
            return
        }
    }

    $granted = $true
    try {
        if ($needGeode) { Install-Geode $gd $payload.MinGeode }

        Write-Step "Installing $title"
        Wait-GDClosed
        $final = Install-ModFile $modsDir $payload
        Write-Ok "Installed and verified: $final"
    } finally {
        if ($elevated) { $granted = Grant-UserAccess $gd }
    }
    if (-not $granted) {
        Stop-Install ("$title was copied, but your Windows account still cannot write to $gd, so Geode cannot load it. " +
            'Move Geometry Dash to a Steam library outside Program Files (Steam > Settings > Storage), then run this installer again.')
    }

    Write-Host ''
    Write-Host "  Done! $title is installed." -ForegroundColor Green
    if ($needGeode) { Write-Host '  The first start with Geode can take a little longer than usual.' }
    Write-Host ''
    if ($elevated) {
        Write-Host '  Start Geometry Dash from Steam as usual once this window is closed.'
        Write-Host ''
        [void](Read-Answer 'Press Enter to close')
    } elseif (Confirm-Choice 'Start Geometry Dash now?' $true) {
        Start-GD $gd
    }
}

try {
    Invoke-Main
} catch {
    $State.Code = 10
    $err = $_
    Write-Host ''
    Write-Host '  Installation failed.' -ForegroundColor Red
    Write-Host "  $($err.Exception.Message)" -ForegroundColor Red
    if (-not $err.Exception.Data['rp']) {
        if (Test-AccessDenied $err) { Write-Host '  Windows denied access to a file. Close Geometry Dash and try again.' -ForegroundColor Yellow }
        if ($err.InvocationInfo -and $err.InvocationInfo.ScriptLineNumber) { Write-Host "  (line $($err.InvocationInfo.ScriptLineNumber))" -ForegroundColor DarkGray }
    }
    Write-Host ''
    Write-Host '  To install by hand: copy the .geode file from the release page into'
    Write-Host '  the geode\mods folder inside your Geometry Dash folder (Geode must be installed).'
    Write-Host ''
    [void](Read-Host '  Press Enter to close')
} finally {
    if ($State.TempDir -and (Test-Dir $State.TempDir)) { Remove-Item -LiteralPath $State.TempDir -Recurse -Force -ErrorAction SilentlyContinue }
}
exit $State.Code
