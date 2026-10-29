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

$preset = switch ($Toolchain) {
    "msvc2026" { "msvc-2026-$($Config.ToLower())" }
    "msvc2022" { "msvc-2022-$($Config.ToLower())" }
    default     { "$Toolchain-$($Config.ToLower())" }
}
Write-Host "Building with preset: $preset"

# Source libraries live in git submodules (see libs/development.md): make
# sure they are present before configuring.
if (-not (Test-Path "$PSScriptRoot\..\libs\lunasvg\CMakeLists.txt") -or
    -not (Test-Path "$PSScriptRoot\..\libs\rnnoise\include\rnnoise.h") -or
    -not (Test-Path "$PSScriptRoot\..\libs\speexdsp\include\speex\speex_echo.h")) {
    Write-Host "Initializing git submodules..."
    git -C "$PSScriptRoot\.." submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

cmake --preset $preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

cmake --build --preset $preset
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Host "Done -> bin\krastor.exe"
