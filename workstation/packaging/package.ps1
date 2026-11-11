param(
    [Parameter(Mandatory=$true)][string]$BuildDir,
    [Parameter(Mandatory=$true)][string]$QtRoot,
    [Parameter(Mandatory=$true)][string]$IsccPath,
    [Parameter(Mandatory=$true)][string]$StageDir,
    [string]$RuntimeDir = "$env:WINDIR\System32"
)
$ErrorActionPreference = 'Stop'
$project = Split-Path -Parent $PSScriptRoot
if (Test-Path -LiteralPath $StageDir) { throw 'Choose a new, empty staging directory.' }
$StageDir = [IO.Path]::GetFullPath($StageDir)
New-Item -ItemType Directory -Path "$StageDir\pc_control\native" -Force | Out-Null
foreach ($name in @('StepWorkstation.exe','StepGcodeConverter.exe','helix_pc.exe')) {
    $target = if ($name -eq 'helix_pc.exe') { "$StageDir\pc_control" } else { "$StageDir\pc_control\native" }
    Copy-Item -LiteralPath (Join-Path $BuildDir $name) -Destination $target
}
& (Join-Path $QtRoot 'bin\windeployqt.exe') --release --no-compiler-runtime --no-translations --no-opengl-sw --no-system-d3d-compiler "$StageDir\pc_control\native\StepWorkstation.exe"
if ($LASTEXITCODE) { throw 'Qt deployment failed.' }
$runtime = @('msvcp140.dll','msvcp140_1.dll','msvcp140_2.dll','msvcp140_atomic_wait.dll','vcruntime140.dll','vcruntime140_1.dll','concrt140.dll')
foreach ($name in $runtime) {
    $source = Join-Path $RuntimeDir $name
    if (-not (Test-Path -LiteralPath $source)) { throw "Missing x64 runtime: $name" }
    Copy-Item -LiteralPath $source -Destination "$StageDir\pc_control\native"
    Copy-Item -LiteralPath $source -Destination "$StageDir\pc_control"
}
foreach ($name in @('README.md','Configure-STEPStudio.cmd','Configure-STEPStudio.ps1','Start-Workstation.cmd','SR175_3200_Helix_Ready_25mms_v4.zip')) {
    Copy-Item -LiteralPath (Join-Path $project $name) -Destination $StageDir
}
foreach ($name in @('examples','licenses')) { Copy-Item -LiteralPath (Join-Path $project $name) -Destination $StageDir -Recurse }
foreach ($name in @('README.md','CONVERSION_GUIDE.md','Open_Pendant.cmd','workstation.paths.example.json')) {
    Copy-Item -LiteralPath (Join-Path $project "pc_control\$name") -Destination "$StageDir\pc_control"
}
$manifest = @()
Get-ChildItem -LiteralPath $StageDir -Recurse -File | Sort-Object FullName | ForEach-Object {
    $manifest += [ordered]@{path=$_.FullName.Substring($StageDir.Length+1).Replace('\','/'); bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
$notice = @{
    version='2026.11.11'; platform='Windows 10/11 x64'; qt_version='6.8.2';
    studio_included=$false; step_sdk_development_files_included=$false;
    data_location='%LOCALAPPDATA%\STEPROBOTICS\StepWorkstation'; files=$manifest
} | ConvertTo-Json -Depth 6
[IO.File]::WriteAllText("$StageDir\MANIFEST.json",$notice,[Text.UTF8Encoding]::new($false))
$releaseDir = Join-Path $project 'releases'
New-Item -ItemType Directory -Path $releaseDir -Force | Out-Null
& $IsccPath "/DStageDir=$StageDir" "/DReleaseDir=$releaseDir" (Join-Path $PSScriptRoot 'StepWorkstation.iss')
if ($LASTEXITCODE) { throw 'Installer build failed.' }
$installer = Get-Item (Join-Path $releaseDir 'StepWorkstation-2026.11.11-win64-Setup.exe')
if ($installer.Length -ge 50000000) { throw 'Installer exceeds the 50,000,000-byte limit.' }
$checksum = (Get-FileHash -LiteralPath $installer.FullName -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + $installer.Name
[IO.File]::WriteAllText((Join-Path $releaseDir 'SHA256SUMS.txt'),$checksum+"`n",[Text.UTF8Encoding]::new($false))
Write-Host ('Installer: '+$installer.FullName+'; '+$installer.Length+' bytes; '+[math]::Round($installer.Length/1MB,2)+' MiB')
