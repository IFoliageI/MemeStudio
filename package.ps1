param(
    [ValidateSet("x64", "arm64")][string]$Architecture = "x64",
    [switch]$SkipBuild
)
$ErrorActionPreference = "Stop"

$projectRoot = $PSScriptRoot
if (-not $SkipBuild) {
    & (Join-Path $projectRoot "build.ps1") -Architecture $Architecture -Configuration Release
}

$binDir = Join-Path $projectRoot "out\bin\$Architecture\Release"
$guiExe = Join-Path $binDir "MemeStudio.exe"
$cliExe = Join-Path $binDir "MemeStudio-cli.exe"
if (-not (Test-Path -LiteralPath $guiExe) -or -not (Test-Path -LiteralPath $cliExe)) {
    throw "Release binaries missing under $binDir. Run .\build.ps1 -Architecture $Architecture first."
}

$stageRoot = Join-Path $projectRoot "out\staging\$Architecture\MemeStudio"
$pkgDir = Join-Path $projectRoot "out\packages"

# Always wipe staging clean to prevent stale cross-arch or historical file leakage!
if (Test-Path -LiteralPath $stageRoot) {
    Remove-Item -LiteralPath $stageRoot -Recurse -Force
}
New-Item -ItemType Directory -Path $stageRoot, "$stageRoot\plugins", "$stageRoot\sdk", "$stageRoot\docs", $pkgDir -Force | Out-Null

# Strict binary & user-documentation whitelist (zero src/, tests/, or compiler intermediates!)
Copy-Item -LiteralPath $guiExe -Destination "$stageRoot\MemeStudio.exe" -Force
Copy-Item -LiteralPath $cliExe -Destination "$stageRoot\MemeStudio-cli.exe" -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "sdk\memestudio_plugin.h") -Destination "$stageRoot\sdk\memestudio_plugin.h" -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "sdk\memestudio_plugin_kit.hpp") -Destination "$stageRoot\sdk\memestudio_plugin_kit.hpp" -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "README.md") -Destination "$stageRoot\README.md" -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "THIRD_PARTY.md") -Destination "$stageRoot\THIRD_PARTY.md" -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "docs\CLI.md") -Destination "$stageRoot\docs\CLI.md" -Force
Copy-Item -LiteralPath (Join-Path $projectRoot "docs\PLUGIN_SDK.md") -Destination "$stageRoot\docs\PLUGIN_SDK.md" -Force

$zipOut = Join-Path $pkgDir "MemeStudio-Windows-$Architecture.zip"
if (Test-Path -LiteralPath $zipOut) {
    Remove-Item -LiteralPath $zipOut -Force
}
& tar.exe -a -c -f $zipOut -C (Split-Path -Parent $stageRoot) "MemeStudio"
if ($LASTEXITCODE -ne 0) {
    throw "Archive creation failed for $zipOut"
}

Write-Host "Package ready: $zipOut"
