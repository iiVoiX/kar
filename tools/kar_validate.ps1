# Release check (RELEASE_PLAN.md 12.3): one race under the Vulkan validation layer for each upscaler, failing on any
# VUID-, THREADING or validation error line. Needs the Vulkan SDK's layer. Uses tools/kar_race_test.ps1 and the
# profile given; restores nothing itself (the race test does its own setup).
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/kar_validate.ps1 -Profile build/pt -Out E:\kirby\ab\val
param([string]$Profile = 'build/pt', [string]$Out = "$PSScriptRoot\..\build\validate",
      [string]$Script = 'kirby/scripts/soak60.txt', [string[]]$Upscalers = @('fsr', 'fsr3', 'external'),
      [int]$MaxMinutes = 12)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bad = 0
foreach ($u in $Upscalers) {
    $dir = Join-Path $Out $u
    & powershell -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\kar_race_test.ps1" -Runs 1 -Script $Script `
        -Profile $Profile -Out $dir -Shots '8400-8400:1' -ShotCount 1 -MaxMinutes $MaxMinutes `
        -Env "KAR_UPSCALE=$u;TOTK_VK_VALIDATION=1" | Out-Null
    $log = Join-Path $dir 'run1.err'
    if (-not (Test-Path $log)) { Write-Host "$u: no log"; $bad++; continue }
    $hits = Select-String -Path $log -Pattern 'VUID-|THREADING ERROR|vk validation error' | Measure-Object
    $used = Select-String -Path $log -Pattern 'FSR 3.1 upscaling|upscaling 1|FSR 1 EASU' -Quiet
    Write-Host ("{0}: {1} validation messages{2}" -f $u, $hits.Count, $(if ($used) { '' } else { ' (upscaler not seen in the log)' }))
    if ($hits.Count) { $bad++ }
}
if ($bad) { Write-Host "validation: $bad of $($Upscalers.Count) failed"; exit 1 }
Write-Host 'validation: clean'
