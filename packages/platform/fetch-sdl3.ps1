# Downloads SDL3 for Windows (MSVC) into vendor/SDL3: a fixed version, checked by its SHA-256.
# Run it once before building a program that uses `platform`:
#     powershell -ExecutionPolicy Bypass -File packages/platform/fetch-sdl3.ps1
# Running it again does nothing when the right version is already there.

$ErrorActionPreference = 'Stop'

$Version = '3.4.18'
$Sha256 = '78d84602ae616cfe26b33a73b7d3b9b6b56e8a6650e53d92c9c6ca938f747acd'
$Name = "SDL3-devel-$Version-VC.zip"
$Url = "https://github.com/libsdl-org/SDL/releases/download/release-$Version/$Name"

$Vendor = Join-Path $PSScriptRoot 'vendor'
$Target = Join-Path $Vendor 'SDL3'
$Marker = Join-Path $Target 'VERSION.txt'

if ((Test-Path $Marker) -and ((Get-Content $Marker -Raw).Trim() -eq $Version)) {
    Write-Host "SDL3 $Version is already in $Target"
    exit 0
}

New-Item -ItemType Directory -Force $Vendor | Out-Null
$Zip = Join-Path $Vendor $Name
Write-Host "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $Zip -UseBasicParsing

$Actual = (Get-FileHash $Zip -Algorithm SHA256).Hash.ToLowerInvariant()
if ($Actual -ne $Sha256) {
    Remove-Item $Zip
    throw "SHA-256 mismatch for ${Name}: expected $Sha256, got $Actual. Nothing was installed."
}

$Extract = Join-Path $Vendor 'extract'
if (Test-Path $Extract) { Remove-Item -Recurse -Force $Extract }
Expand-Archive -Path $Zip -DestinationPath $Extract
if (Test-Path $Target) { Remove-Item -Recurse -Force $Target }
Move-Item (Join-Path $Extract "SDL3-$Version") $Target
Set-Content -Path $Marker -Value $Version -Encoding ascii
Remove-Item -Recurse -Force $Extract, $Zip

Write-Host "SDL3 $Version installed in $Target"
