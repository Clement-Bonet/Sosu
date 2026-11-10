# Installs sosuc for the current user on Windows x64, without administrator rights.
#   irm https://github.com/Clement-Bonet/Sosu/releases/latest/download/install.ps1 | iex
#   & ([scriptblock]::Create((irm .../install.ps1))) -Version 0.6.0     a given version
#   & ([scriptblock]::Create((irm .../install.ps1))) -Uninstall
#   .\install.ps1 -Archive sosu-0.6.0-windows-x64.zip                   an archive already here
#
# It downloads the archive of the release, checks it against the SHA256SUMS.txt of the release,
# puts it into %LOCALAPPDATA%\Programs\Sosu and adds its bin\ to the user's PATH. -Uninstall
# removes both. $env:SOSU_RELEASES replaces https://github.com/Clement-Bonet/Sosu/releases.
param(
    [string]$Version = '',
    [string]$Archive = '',
    [switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'  # Invoke-WebRequest is much slower with its progress bar

$app = Join-Path $env:LOCALAPPDATA 'Programs\Sosu'
$bin = Join-Path $app 'bin'

# The user's PATH, as written in the registry: %VARIABLES% stay as they are.
function Get-UserPath {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment')
    $value = $key.GetValue('Path', '', [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
    $key.Close()
    return $value
}

function Set-UserPath([string]$Value) {
    $key = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Environment', $true)
    $key.SetValue('Path', $Value, [Microsoft.Win32.RegistryValueKind]::ExpandString)
    $key.Close()
    # Setting a variable through .NET tells the open programs that the environment changed.
    [Environment]::SetEnvironmentVariable('SOSU_INSTALL', '1', 'User')
    [Environment]::SetEnvironmentVariable('SOSU_INSTALL', $null, 'User')
}

function Get-OtherEntries([string]$Paths) {
    return @($Paths -split ';' | Where-Object { $_ -ne '' -and $_.TrimEnd('\') -ne $bin })
}

if (Test-Path (Join-Path $app 'unins000.exe')) {
    throw "Sosu was installed in $app by its setup: uninstall it from Installed apps first"
}

if ($Uninstall) {
    Set-UserPath ((Get-OtherEntries (Get-UserPath)) -join ';')
    if (Test-Path $app) { Remove-Item -Recurse -Force $app }
    Write-Output "Sosu is uninstalled from $app."
    return
}

$work = Join-Path ([IO.Path]::GetTempPath()) ("sosu-install-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $work | Out-Null
try {
    if ($Archive -eq '') {
        # Windows PowerShell 5.1 may not offer TLS 1.2 by itself.
        [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor 3072
        $releases = if ($env:SOSU_RELEASES) { $env:SOSU_RELEASES } else { 'https://github.com/Clement-Bonet/Sosu/releases' }
        if ($Version -eq '') {
            $latest = Invoke-RestMethod 'https://api.github.com/repos/Clement-Bonet/Sosu/releases/latest'
            $Version = $latest.tag_name
        }
        $Version = $Version.TrimStart('v')
        $name = "sosu-$Version-windows-x64.zip"
        $Archive = Join-Path $work $name
        Write-Output "Downloading $name"
        Invoke-WebRequest -UseBasicParsing "$releases/download/v$Version/$name" -OutFile $Archive
        $sums = Join-Path $work 'SHA256SUMS.txt'
        Invoke-WebRequest -UseBasicParsing "$releases/download/v$Version/SHA256SUMS.txt" -OutFile $sums
        $line = Get-Content $sums | Where-Object { $_ -match "^([0-9a-f]{64}) [ *]?$([regex]::Escape($name))$" } |
            Select-Object -First 1
        $got = (Get-FileHash -Algorithm SHA256 $Archive).Hash.ToLower()
        if (-not $line -or $line.Substring(0, 64) -ne $got) {
            throw "the SHA-256 of $name is not the one of SHA256SUMS.txt"
        }
    }
    Expand-Archive -Path $Archive -DestinationPath $work
    $source = Get-ChildItem $work -Directory | Where-Object { Test-Path (Join-Path $_.FullName 'bin\sosuc.exe') } |
        Select-Object -First 1
    if (-not $source) { throw "the archive has no bin\sosuc.exe" }

    # A new version replaces the old one whole: no file of the old library stays behind.
    if (Test-Path $app) { Remove-Item -Recurse -Force $app }
    New-Item -ItemType Directory -Force $app | Out-Null
    Copy-Item -Recurse (Join-Path $source.FullName '*') $app
} finally {
    Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue
}

$others = Get-OtherEntries (Get-UserPath)
Set-UserPath ((@($others) + $bin) -join ';')
if (-not (($env:Path -split ';') -contains $bin)) { $env:Path = "$env:Path;$bin" }

$installed = (& (Join-Path $bin 'sosuc.exe') --version) -replace '^sosuc ', ''
Write-Output "Sosu $installed is installed in $app, and $bin is in your PATH."
Write-Output "The packages are in $app\packages."
Write-Output 'sosuc needs MSVC to build programs: Visual Studio or its Build Tools, with "Desktop development with C++".'
