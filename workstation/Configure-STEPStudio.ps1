param([string]$StudioRoot, [string]$SimulationSeed)
$ErrorActionPreference = 'Stop'
if (-not $StudioRoot) { $StudioRoot = Read-Host 'STEPStudio root (contains RobotController_PC)' }
if (-not $SimulationSeed) { $SimulationSeed = Read-Host 'SR175-3200 workstation folder (contains home and SRSystemFile)' }
$StudioRoot = (Resolve-Path -LiteralPath $StudioRoot).Path
$SimulationSeed = (Resolve-Path -LiteralPath $SimulationSeed).Path
foreach ($name in @('1-NonReal.exe','2-RobotMoveControl.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $StudioRoot "RobotController_PC\Environment\$name"))) { throw "Missing controller: $name" }
}
foreach ($name in @('home','SRSystemFile')) {
    if (-not (Test-Path -LiteralPath (Join-Path $SimulationSeed $name) -PathType Container)) { throw "Missing workstation folder: $name" }
}
$configuration = @{studio_root=$StudioRoot; simulation_seed=$SimulationSeed; data_root=''} | ConvertTo-Json
# Qt expects UTF-8 JSON without a BOM, including when this script runs on Windows PowerShell 5.1.
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'pc_control\workstation.paths.json'), $configuration, [Text.UTF8Encoding]::new($false))
Write-Host 'Configuration saved. Restart StepWorkstation. No controller has been started or connected.'
