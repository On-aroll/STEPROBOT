# Move files/directories to the Windows Recycle Bin (never hard-delete).
# Safe deletion helper for project cleanup.
#
# Usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File recycle-delete.ps1 -Path "D:\a","D:\b"
#
# Notes:
#  - Uses the shell Recycle Bin API, so items stay recoverable.
#  - Refuses to touch drive roots or known system directories.

param(
  [Parameter(Mandatory = $true)]
  [string[]] $Path
)

Add-Type -AssemblyName Microsoft.VisualBasic

$blocked = @('C:', 'D:', 'C:\Windows', 'C:\Users', 'C:\Program Files', 'C:\Program Files (x86)')

foreach ($p in $Path) {
  $full = [System.IO.Path]::GetFullPath($p)
  $norm = $full.TrimEnd('\')

  $isBlocked = $false
  foreach ($b in $blocked) { if ($norm -ieq $b.TrimEnd('\')) { $isBlocked = $true } }
  if ($isBlocked) { Write-Output "[BLOCKED] protected path: $full"; continue }

  if (-not (Test-Path -LiteralPath $full)) { Write-Output "[SKIP] not found: $full"; continue }

  $item = Get-Item -LiteralPath $full -Force
  try {
    if ($item.PSIsContainer) {
      [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteDirectory(
        $full, [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs,
        [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
    } else {
      [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile(
        $full, [Microsoft.VisualBasic.FileIO.UIOption]::OnlyErrorDialogs,
        [Microsoft.VisualBasic.FileIO.RecycleOption]::SendToRecycleBin)
    }
    Write-Output "[RECYCLED] $full"
  } catch {
    Write-Output "[ERROR] $full : $($_.Exception.Message)"
  }
}

Write-Output "done."
