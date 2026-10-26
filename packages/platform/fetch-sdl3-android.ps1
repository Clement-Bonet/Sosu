# Downloads SDL3 for Android into vendor/SDL3-android (spec §11.5): a fixed version, checked by
# its SHA-256. It keeps the Android archive of SDL (`SDL3-<version>.aar`: its Java classes and
# libraries, for the Gradle project) and `lib/<abi>/libSDL3.so` (to link `libmain.so`).
# Run it once before building for Android:
#     powershell -ExecutionPolicy Bypass -File packages/platform/fetch-sdl3-android.ps1
# Running it again does nothing when the right version is already there.

$ErrorActionPreference = 'Stop'

$Version = '3.4.18'
$Sha256 = 'e09e4d6593335d089ba124375e8b8abd0fd75173dffcd2c17012b45af121d4e9'
$Name = "SDL3-devel-$Version-android.zip"
$Url = "https://github.com/libsdl-org/SDL/releases/download/release-$Version/$Name"

$Vendor = Join-Path $PSScriptRoot 'vendor'
$Target = Join-Path $Vendor 'SDL3-android'
$Marker = Join-Path $Target 'VERSION.txt'

if ((Test-Path $Marker) -and ((Get-Content $Marker -Raw).Trim() -eq $Version)) {
    Write-Host "SDL3 $Version for Android is already in $Target"
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

$Extract = Join-Path $Vendor 'extract-android'
if (Test-Path $Extract) { Remove-Item -Recurse -Force $Extract }
Expand-Archive -Path $Zip -DestinationPath $Extract
$Aar = Get-ChildItem -Path $Extract -Recurse -Filter "SDL3-*.aar" | Select-Object -First 1
if (-not $Aar) { throw "no SDL3-*.aar in $Name" }

# the .aar is a zip: its libraries are under prefab/modules/SDL3-shared/libs/android.<abi>/
$Inside = Join-Path $Extract 'aar'
$AarZip = Join-Path $Extract 'SDL3.zip'
Copy-Item $Aar.FullName $AarZip
Expand-Archive -Path $AarZip -DestinationPath $Inside

if (Test-Path $Target) { Remove-Item -Recurse -Force $Target }
New-Item -ItemType Directory -Force $Target | Out-Null
Copy-Item $Aar.FullName (Join-Path $Target "SDL3-$Version.aar")
foreach ($Abi in 'arm64-v8a', 'x86_64') {
    $Library = Get-ChildItem -Path $Inside -Recurse -Filter 'libSDL3.so' | Where-Object { $_.FullName -match "android\.$Abi" } |
        Select-Object -First 1
    if (-not $Library) { throw "no libSDL3.so for $Abi in $($Aar.Name)" }
    $Dir = Join-Path $Target "lib\$Abi"
    New-Item -ItemType Directory -Force $Dir | Out-Null
    Copy-Item $Library.FullName $Dir
}
Set-Content -Path $Marker -Value $Version -Encoding ascii
Remove-Item -Recurse -Force $Extract, $Zip
Write-Host "SDL3 $Version for Android is in $Target"
