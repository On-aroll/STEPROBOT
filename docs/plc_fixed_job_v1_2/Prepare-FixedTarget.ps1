param(
    [Parameter(Mandatory=$true)][string]$Project,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [switch]$ExistingMappingConfirmed,
    [ValidateRange(1,255)][int]$CaptureNumber = 1,
    [ValidateRange(1,255)][int]$PathNumber = 2,
    [ValidateRange(1,255)][int]$ReturnNumber = 3
)
$ErrorActionPreference = 'Stop'
if (-not $ExistingMappingConfirmed) { throw 'Confirm the actual existing cabinet project and bindings before generating a configured bundle.' }
if ($Project -cnotmatch '^pcjob_[a-z0-9_]{3,39}\.sr$' -or $Project.Length -lt 12 -or $Project.Length -gt 48) { throw 'Use the exact existing lowercase pcjob_*.sr name, not an example or guessed upload time.' }
if ($CaptureNumber -eq $PathNumber -or $CaptureNumber -eq $ReturnNumber -or $PathNumber -eq $ReturnNumber) { throw 'The three existing program numbers must differ.' }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Output already exists; inspect it before creating another bundle.' }
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
foreach ($source in Get-ChildItem -LiteralPath $PSScriptRoot -Filter '*.st' -File) {
    $text = [IO.File]::ReadAllText($source.FullName)
    if ($source.Name -eq 'GVL_FileBridge.st') {
        foreach ($pair in @(
            @('xFixedMappingConfirmed : BOOL := FALSE;', 'xFixedMappingConfirmed : BOOL := TRUE;'),
            @("sFixedProject : STRING(64) := '';", "sFixedProject : STRING(64) := '$Project';"),
            @('uiFixedCapture : UINT := UINT#1;', "uiFixedCapture : UINT := UINT#$CaptureNumber;"),
            @('uiFixedPath : UINT := UINT#2;', "uiFixedPath : UINT := UINT#$PathNumber;"),
            @('uiFixedReturn : UINT := UINT#3;', "uiFixedReturn : UINT := UINT#$ReturnNumber;")
        )) {
            if (-not $text.Contains($pair[0])) { throw ('Unexpected configuration source: ' + $pair[0]) }
            $text = $text.Replace($pair[0], $pair[1])
        }
    }
    [IO.File]::WriteAllText((Join-Path $OutputDirectory $source.Name), $text, $utf8)
    if ($text -match '^\s*(PROGRAM|FUNCTION_BLOCK|FUNCTION)\s') {
        $sections = [regex]::Matches($text, '(?m)^END_VAR\s*$')
        if (-not $sections.Count) { throw ('Missing POU declarations: ' + $source.Name) }
        $last = $sections[$sections.Count-1]
        $declaration = $text.Substring(0, $last.Index + $last.Length).TrimEnd()
        $implementation = [regex]::Replace($text.Substring($last.Index + $last.Length).Trim(), '\s*END_(PROGRAM|FUNCTION_BLOCK|FUNCTION)\s*$', '')
        [IO.File]::WriteAllText((Join-Path $OutputDirectory ($source.BaseName + '_声明.txt')), $declaration + "`r`n", $utf8)
        [IO.File]::WriteAllText((Join-Path $OutputDirectory ($source.BaseName + '_实现.txt')), $implementation.TrimEnd() + "`r`n", $utf8)
    }
}
$profile = [ordered]@{
    schema=1; project=$Project; capture_number=$CaptureNumber; path_number=$PathNumber; return_number=$ReturnNumber
    existing_mapping_confirmed=$true; fixed_path_program='helix_25mms'
    note='Reuses existing cabinet bindings; this file does not create or modify them.'
}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'fixed_job_target.json'), ($profile | ConvertTo-Json) + "`r`n", $utf8)
Write-Output ('Offline bundle created: ' + $OutputDirectory)
Write-Output 'No PLC/robot connection was made. Field compilation and reload validation remain required.'
