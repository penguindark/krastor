# Krastor build helper (Windows).
# Usage:
#   scripts\build.ps1 -Toolchain msvc2026 -Config Debug     (default)
#   scripts\build.ps1 -Toolchain msvc2022 -Config Release
#   scripts\build.ps1 -Toolchain mingw   -Config Debug
param(
    [ValidateSet("msvc2026", "msvc2022", "mingw")]
    [string]$Toolchain = "msvc2026",
    [ValidateSet("Debug", "Release")]
    [string]$Config = "Debug"
)

$preset = "$Toolchain-$($Config.ToLower())"
Write-Host "Building with preset: $preset"

cmake --preset $preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Done -> bin\krastor.exe"
