param(
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
    [ValidateSet("Release", "Debug")][string]$Configuration = "Release",
    [switch]$Test
)
$ErrorActionPreference = "Stop"

function Resolve-CMakeBinDir {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) {
        return Split-Path -Parent $cmd.Source
    }
    $vswhereCandidates = @(
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\Installer\vswhere.exe"
    )
    foreach ($vsw in $vswhereCandidates) {
        if (Test-Path -LiteralPath $vsw) {
            $inst = & $vsw -latest -prerelease -products * -property installationPath
            if ($inst) {
                $bundled = Join-Path $inst "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin"
                if (Test-Path -LiteralPath (Join-Path $bundled "cmake.exe")) {
                    return $bundled
                }
            }
        }
    }
    throw "CMake 3.24+ not found. Install Visual Studio Desktop development with C++ or add cmake to PATH."
}

$cmakeBin = Resolve-CMakeBinDir
$cmake = Join-Path $cmakeBin "cmake.exe"
$ctest = Join-Path $cmakeBin "ctest.exe"
$preset = "windows-$Architecture"

Push-Location $PSScriptRoot
try {
    & $cmake --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $preset." }

    & $cmake --build "out/build/$Architecture" --config $Configuration
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed for $Architecture ($Configuration)." }

    if ($Test) {
        if ($Architecture -ne "x64" -and $env:PROCESSOR_ARCHITECTURE -ne "ARM64") {
            Write-Host "Skipping runtime CTest execution for cross-compiled target '$Architecture' on '$env:PROCESSOR_ARCHITECTURE' host."
        } else {
            & $ctest --test-dir "out/build/$Architecture" -C $Configuration --output-on-failure
            if ($LASTEXITCODE -ne 0) { throw "Automated test suite failed." }
        }
    }

    $binDir = Join-Path $PSScriptRoot "out\bin\$Architecture\$Configuration"
    Write-Host "Build succeeded: $(Join-Path $binDir 'MemeStudio.exe')"
    Write-Host "Build succeeded: $(Join-Path $binDir 'MemeStudio-cli.exe')"
} finally {
    Pop-Location
}
