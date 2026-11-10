# Tries the Windows installer for real, silently: install, PATH, the program of the README built
# by the installed sosuc, then uninstall.
#   powershell -File installers\windows\test-setup.ps1 sosu-0.6.0-windows-x64-setup.exe
# It installs into %LOCALAPPDATA%\Programs\Sosu and refuses to run when Sosu is already there.
param([Parameter(Mandatory = $true)][string]$Setup)
$ErrorActionPreference = 'Stop'

$app = Join-Path $env:LOCALAPPDATA 'Programs\Sosu'
$bin = Join-Path $app 'bin'
if (Test-Path $app) { throw "Sosu is already installed in ${app}: uninstall it first" }

function UserPath { (Get-ItemProperty -Path 'HKCU:\Environment' -Name Path -ErrorAction SilentlyContinue).Path }
function HasBin { (";" + (UserPath) + ";").ToUpper().Contains(";" + $bin.ToUpper() + ";") }

$log = Join-Path $env:TEMP 'sosu-setup.log'
Start-Process -Wait -FilePath (Resolve-Path $Setup) -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=$log"
if (-not (Test-Path "$bin\sosuc.exe")) { Get-Content $log; throw "sosuc.exe is not in $bin" }
if (-not (HasBin)) { throw "$bin is not in the user's PATH" }
Write-Output "installed: $(& "$bin\sosuc.exe" --version)"

$readme = Join-Path $env:TEMP 'sosu-setup-readme'
Remove-Item -Recurse -Force $readme -ErrorAction SilentlyContinue
Copy-Item -Recurse (Join-Path $PSScriptRoot '..\..\tests\programs\readme') $readme
Remove-Item -Recurse -Force "$readme\build" -ErrorAction SilentlyContinue
# `sosuc` alone, found in the PATH as a new terminal would find it.
$env:Path = "$bin;$env:Path"
$got = (& sosuc run $readme) -join "`n"
if ($LASTEXITCODE -ne 0) { throw "sosuc run failed" }
$expected = (Get-Content "$readme\expected.txt") -join "`n"
if ($got -ne $expected) { throw "got:`n$got`nexpected:`n$expected" }
Write-Output "the program of the README runs"

# The uninstaller copies itself elsewhere and returns at once: wait until the folder is gone.
Start-Process -Wait -FilePath "$app\unins000.exe" -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART'
for ($i = 0; $i -lt 60 -and (Test-Path "$bin\sosuc.exe"); $i++) { Start-Sleep -Seconds 1 }
if (Test-Path "$bin\sosuc.exe") { throw "the uninstaller left $bin\sosuc.exe" }
for ($i = 0; $i -lt 10 -and (HasBin); $i++) { Start-Sleep -Seconds 1 }
if (HasBin) { throw "the uninstaller left $bin in the user's PATH" }
Remove-Item -Recurse -Force $readme
Write-Output "uninstalled"
