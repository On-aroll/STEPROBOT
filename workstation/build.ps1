param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build'),
    [string]$SdkDir,
    [string]$SdkBridge,
    [string]$ModeHelper
)
$ErrorActionPreference = 'Stop'
$sourceDir = Join-Path $PSScriptRoot 'pc_control'
if (-not (Test-Path -LiteralPath (Join-Path $sourceDir 'assets/sr175.mesh'))) {
    throw 'Restore the licensed local model first: python pc_control/tools/prepare_assets.py --source <sr175.mesh>'
}
$env:PATH = (Join-Path $QtRoot 'bin') + ';' + $env:PATH
$configureArgs = @('-S', $sourceDir, '-B', $BuildDir, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', "-DCMAKE_PREFIX_PATH=$QtRoot")
if ($SdkDir) { $configureArgs += "-DSTEP_SDK_DIR=$SdkDir" }
& cmake @configureArgs
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& cmake --build $BuildDir
if ($LASTEXITCODE) { throw 'Build failed' }
& ctest --test-dir $BuildDir --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
$deployDir = Join-Path $sourceDir 'native'
New-Item -ItemType Directory -Path $deployDir -Force | Out-Null
foreach ($name in @('StepWorkstation.exe','StepGcodeConverter.exe')) {
    Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination $deployDir -Force
}
Copy-Item -LiteralPath (Join-Path $sourceDir 'assets') -Destination $deployDir -Recurse -Force
if ($SdkBridge) {
    Copy-Item -LiteralPath $SdkBridge -Destination (Join-Path $deployDir 'helix_pc.exe') -Force
} elseif (Test-Path -LiteralPath (Join-Path $BuildDir 'helix_pc.exe')) {
    Copy-Item -LiteralPath (Join-Path $BuildDir 'helix_pc.exe') -Destination $deployDir -Force
}
if ($ModeHelper) {
    $modeDeployDir = Join-Path $deployDir 'mode_helper'
    New-Item -ItemType Directory -Path $modeDeployDir -Force | Out-Null
    Copy-Item -Path (Join-Path $ModeHelper '*') -Destination $modeDeployDir -Recurse -Force
}
& (Join-Path $QtRoot 'bin/windeployqt.exe') --release --no-translations (Join-Path $deployDir 'StepWorkstation.exe')
if ($LASTEXITCODE) { throw 'Qt deployment failed' }
Write-Host 'R6.4 built, tested and deployed. Supply optional vendor SDK DLLs locally; see README.md.'
