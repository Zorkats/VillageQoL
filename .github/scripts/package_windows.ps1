<#
.SYNOPSIS
    Assemble the VillageQoL package from a finished build tree.

.DESCRIPTION
    Writes VillageQoL-<version>.zip to OutDir: mods\VillageQoL.dll in the game folder layout,
    with the license. Canopy Studio's installer downloads it by exactly this name and reads
    mods/VillageQoL.dll from it, so neither may change. Nothing is written until Canopy's
    check_windows_deps.py, from the submodule, confirms the DLL links the MSVC runtime
    statically.

.EXAMPLE
    pwsh .github/scripts/package_windows.ps1 -BuildDir build -Version 1.2.0
#>

param(
    [Parameter(Mandatory = $true)][string] $BuildDir,
    [Parameter(Mandatory = $true)][string] $Version,
    [string] $OutDir = "dist"
)

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

$dll = Join-Path $BuildDir "src/VillageQoL.dll"
if (-not (Test-Path $dll)) {
    Write-Error "build tree is missing $dll`nbuild first: cmake --build $BuildDir"
}
python (Join-Path $repoRoot "external/Canopy/.github/scripts/check_windows_deps.py") $dll
if ($LASTEXITCODE -ne 0) {
    Write-Error "packaging refused: VillageQoL.dll needs a DLL the game folder does not have"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$stage = Join-Path $OutDir "stage-windows"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
cmake --install $BuildDir --component villageqol --prefix $stage
if ($LASTEXITCODE -ne 0) {
    Write-Error "cmake --install of villageqol failed"
}
$zip = Join-Path $OutDir "VillageQoL-$Version.zip"
if (Test-Path $zip) { Remove-Item -Force $zip }
Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip
Remove-Item -Recurse -Force $stage

Write-Host ""
# Computed through .NET rather than Get-FileHash, which is missing from some constrained hosts.
$stream = [System.IO.File]::OpenRead((Resolve-Path $zip))
try {
    $digest = [System.Security.Cryptography.SHA256]::Create().ComputeHash($stream)
    "{0}  {1}" -f ([BitConverter]::ToString($digest) -replace '-', '').ToLower(), (Split-Path -Leaf $zip)
} finally {
    $stream.Dispose()
}
