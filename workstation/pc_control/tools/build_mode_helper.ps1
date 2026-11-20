param(
    [string]$Output = "$PSScriptRoot\..\..\..\tmp\workstation_ui_r6_4_build\mode_helper",
    [Parameter(Mandatory=$true)][string]$Vendor
)
$ErrorActionPreference = 'Stop'
$modeCompiler = "$env:WINDIR\Microsoft.NET\Framework64\v4.0.30319\csc.exe"
$modeSource = (Resolve-Path "$PSScriptRoot\..\mode_helper").Path
$modeTests = (Resolve-Path "$PSScriptRoot\..\tests\mode_helper_test.cs").Path
New-Item -ItemType Directory -Force -Path $Output | Out-Null
$modeOutput = (Resolve-Path $Output).Path
& $modeCompiler /nologo /target:exe /platform:x64 /codepage:65001 /optimize+ /warn:4 /reference:System.dll /reference:System.Core.dll /reference:System.Web.Extensions.dll "/reference:$Vendor\Robot-SDK-CSharp.dll" "/out:$modeOutput\StepModeSwitch.exe" "$modeSource\ModeSwitchCore.cs" "$modeSource\Program.cs"
if ($LASTEXITCODE -ne 0) { throw 'Mode helper compilation failed' }
foreach ($modeDependency in @('Robot-SDK-CSharp.dll','msvcp140.dll','vcruntime140.dll','vcruntime140_1.dll')) {
    Copy-Item -LiteralPath "$Vendor\$modeDependency" -Destination $modeOutput -Force
}
'<?xml version="1.0"?><configuration><startup useLegacyV2RuntimeActivationPolicy="true"><supportedRuntime version="v4.0" sku=".NETFramework,Version=v4.0"/></startup></configuration>' | Set-Content -LiteralPath "$modeOutput\StepModeSwitch.exe.config" -Encoding UTF8
& $modeCompiler /nologo /target:exe /platform:x64 /codepage:65001 /optimize+ /warn:4 "/out:$modeOutput\ModeCoreTest.exe" "$modeSource\ModeSwitchCore.cs" $modeTests
if ($LASTEXITCODE -ne 0) { throw 'Mode test compilation failed' }
& "$modeOutput\ModeCoreTest.exe"
if ($LASTEXITCODE -ne 0) { throw 'Mode core tests failed' }
