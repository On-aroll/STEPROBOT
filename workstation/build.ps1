param(
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [string]$BuildDir = (Join-Path $PSScriptRoot 'build')
)
$ErrorActionPreference = 'Stop'
# Run in an MSVC 2022 x64 developer PowerShell / Native Tools environment.
& cmake -S (Join-Path $PSScriptRoot 'pc_control') -B $BuildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_PREFIX_PATH=$QtRoot"
if ($LASTEXITCODE) { throw 'CMake configure failed' }
& cmake --build $BuildDir
if ($LASTEXITCODE) { throw 'Build failed' }
& ctest --test-dir $BuildDir --output-on-failure
if ($LASTEXITCODE) { throw 'Tests failed' }
New-Item -ItemType Directory -Path (Join-Path $PSScriptRoot 'pc_control\native') -Force | Out-Null
foreach ($name in @('StepWorkstation.exe','StepGcodeConverter.exe')) {
    Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination (Join-Path $PSScriptRoot 'pc_control\native')
}
& (Join-Path $QtRoot 'bin\windeployqt.exe') --release --no-translations --no-opengl-sw --no-system-d3d-compiler (Join-Path $PSScriptRoot 'pc_control\native\StepWorkstation.exe')
if ($LASTEXITCODE) { throw 'Qt deployment failed' }
Write-Host 'Build complete. STEP SDK 通信已移除，上位机现通过 Modbus TCP 与机器人从站交互。'
