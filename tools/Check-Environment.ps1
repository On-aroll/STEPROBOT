param(
    [string]$ProjectRoot = (Split-Path $PSScriptRoot -Parent),
    [string]$ReportDirectory
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
if (-not $ReportDirectory) { $ReportDirectory = Join-Path $ProjectRoot 'reports\environment-check' }
$checks = New-Object 'System.Collections.Generic.List[object]'

function Add-Check([string]$Name, [string]$Status, [string]$Detail) {
    $checks.Add([pscustomobject]@{ Name = $Name; Status = $Status; Detail = $Detail })
    Write-Host ('[{0}] {1}: {2}' -f $Status, $Name, $Detail)
}
function Run-Local([string]$File, [string[]]$Arguments) {
    $saved = $ErrorActionPreference
    try {
        # Some programs write informational notices to stderr even on success.
        $ErrorActionPreference = 'Continue'
        $output = (& $File @Arguments 2>&1 | ForEach-Object { $_.ToString() } | Out-String).Trim()
        $code = $LASTEXITCODE
        return [pscustomobject]@{ Code = $code; Output = $output }
    } finally { $ErrorActionPreference = $saved }
}
function Check-Command([string]$Name, [string]$File, [string[]]$Arguments, [string]$Expected = '') {
    if (-not (Test-Path -LiteralPath $File -PathType Leaf)) {
        Add-Check $Name 'FAIL' "Missing executable: $File"
        return
    }
    try {
        $result = Run-Local $File $Arguments
        if ($result.Code -eq 0 -and (-not $Expected -or $result.Output -match $Expected)) {
            Add-Check $Name 'PASS' $result.Output
        } else { Add-Check $Name 'FAIL' "Exit $($result.Code): $($result.Output)" }
    } catch { Add-Check $Name 'FAIL' $_.Exception.Message }
}

Write-Host 'STEPROBOT local development check (no controller connection or motion).'
Write-Host "Project: $ProjectRoot"
Check-Command 'DotNet.SDK8' 'C:\Program Files\dotnet\dotnet.exe' @('--list-sdks') '(?m)^8\.'
Check-Command 'DotNet.Runtime8' 'C:\Program Files\dotnet\dotnet.exe' @('--list-runtimes') '(?m)^Microsoft\.NETCore\.App 8\.'

$vcvars = 'D:\VSBuildTools\VC\Auxiliary\Build\vcvars64.bat'
$compiler = @(Get-ChildItem 'D:\VSBuildTools\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe' -ErrorAction SilentlyContinue)
if ((Test-Path -LiteralPath $vcvars) -and $compiler.Count -gt 0) {
    Add-Check 'MSVC.Files' 'PASS' $compiler[0].FullName
} else { Add-Check 'MSVC.Files' 'FAIL' 'Missing x64 MSVC compiler or vcvars64.bat in D:\VSBuildTools.' }

$git = 'D:\Git\cmd\git.exe'
Check-Command 'Git.Program' $git @('--version') '^git version '
Check-Command 'Git.Objects' $git @('-C', $ProjectRoot, 'fsck', '--full', '--no-dangling')

$sdk = Join-Path $ProjectRoot '_extract\sdk\SR.SDK_C++_X64_Win_V2.4.1.20250801'
try {
    $baseline = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'sdk-baseline.json') -Raw | ConvertFrom-Json
    if (@($baseline).Count -ne 5) { throw 'Expected five vendor SDK baseline entries.' }
    $mismatches = @(foreach ($entry in $baseline) {
        $file = Join-Path $sdk $entry.Name
        if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { "Missing $($entry.Name)" }
        elseif ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $entry.Sha256) { "Hash mismatch: $($entry.Name)" }
    })
    if ($mismatches.Count) { Add-Check 'SDK.Files' 'FAIL' ($mismatches -join '; ') }
    else { Add-Check 'SDK.Files' 'PASS' 'All five files match the restored vendor SDK SHA-256 baseline.' }
} catch { Add-Check 'SDK.Files' 'FAIL' $_.Exception.Message }

$hostDir = Join-Path $ProjectRoot '实验准备\上位机\bin\host'
$required = @('StepRobot.App.exe', 'StepRobot.App.dll', 'StepRobot.App.deps.json',
    'StepRobot.App.runtimeconfig.json', 'StepRobot.Native.dll', 'test-fixtures\Robot-SDK-CSharp.dll')
$missing = @($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $hostDir $_) -PathType Leaf) })
if ($missing.Count) { Add-Check 'Host.Files' 'FAIL' ('Missing: ' + ($missing -join ', ')) }
else { Add-Check 'Host.Files' 'PASS' $hostDir }
Check-Command 'Host.NativeSDK' (Join-Path $hostDir 'StepRobot.App.exe') @('--diagnose') 'SDK-V2\.4\.1-20250801F_20251013'

Add-Check 'Studio.Simulation' 'MANUAL' 'User installs STEPStudio. License, station loading, controller startup and live SDK capture are not checked here.'
Add-Check 'Python.Research' 'MANUAL' 'User installs Anaconda. Select its interpreter and run env_check.py separately when ready.'
$failures = @($checks | Where-Object Status -eq 'FAIL').Count
$report = [pscustomobject]@{
    CheckedAt = (Get-Date).ToString('o'); Project = $ProjectRoot
    DevelopmentChecksPassed = ($failures -eq 0); FailureCount = $failures
    SimulationVerified = $false; Checks = @($checks.ToArray())
}
New-Item -ItemType Directory -Path $ReportDirectory -Force | Out-Null
$stem = Join-Path $ReportDirectory ('environment-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$stem.json" -Encoding UTF8
$lines = @('# Local environment check', '', "Checked: $($report.CheckedAt)", '',
    "Development checks passed: $($report.DevelopmentChecksPassed)",
    'Simulation verified: false (separate acceptance required)', '', '| Check | Result | Detail |', '|---|---|---|')
foreach ($item in $checks) {
    $detail = ($item.Detail -replace '\r?\n', '<br>') -replace '\|', '\|'
    $lines += "| $($item.Name) | $($item.Status) | $detail |"
}
$lines | Set-Content -LiteralPath "$stem.md" -Encoding UTF8
Write-Host "Report: $stem.md"
Write-Host 'PASS covers local development only. MANUAL checks remain separate.'
if ($failures) { exit 1 }
exit 0
