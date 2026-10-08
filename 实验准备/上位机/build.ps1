param([switch]$SkipTests)
$ErrorActionPreference = 'Stop'
Push-Location $PSScriptRoot
try {
    & .\native\build.bat
    if ($LASTEXITCODE -ne 0) { throw 'Native bridge build failed.' }
    dotnet build StepRobotHost.sln -c Release --nologo
    if ($LASTEXITCODE -ne 0) { throw 'C# build failed.' }
    if (-not $SkipTests) {
        dotnet test StepRobotHost.sln -c Release --no-build --nologo
        if ($LASTEXITCODE -ne 0) { throw 'Tests failed; publish cancelled.' }
    }
    dotnet publish src/StepRobot.App/StepRobot.App.csproj -c Release --no-build --no-restore -o bin/host --nologo
    if ($LASTEXITCODE -ne 0) { throw 'Publish failed.' }
    Write-Host ('Ready: ' + (Join-Path $PSScriptRoot 'bin\host\StepRobot.App.exe'))
}
finally { Pop-Location }
