# Runs N Kirby Air Riders test races one after another and scores their screenshots (tools/kar_shot_check.py).
#
#   powershell -File tools\kar_race_test.ps1 -Runs 5 -Script <input script> [-Profile <dir>] [-Out <dir>] [-Env 'K=V;K2=V2']
#
# Each run: kar.exe from build\kar458752 with the input script (toRace.txt reaches a race with the old-save test
# profile build\profile-kar-test), screenshots TOTK_SHOTS (default 3000-4200:300) into <Out>\run<k>, stderr into
# <Out>\run<k>.err; the game is closed through its window (never killed: that loses the pipeline cache). Refuses to
# start while another game (WardogsClient) or kar.exe runs. Exit code: the checker's (1 when a run looks broken).
param(
    [int]$Runs = 5,
    [Parameter(Mandatory = $true)][string]$Script,
    [string]$Profile = '',
    [string]$Out = '',
    [string]$Shots = '3000-4200:300',
    [int]$ShotCount = 5,
    [string]$Env = '',
    [long]$Affinity = 0,   # CPU mask for kar.exe (0: all cores), e.g. 1 = only the first core
    [int]$AffinityDelay = 0,   # seconds after the start before the mask is applied
    [int]$MaxMinutes = 20,   # a run is closed after this long at the latest (soak tests: 60+)
    [string]$Exe = ''   # another kar.exe (e.g. an A/B build with --out); default build\kar458752\kar.exe
)
# PowerShell 5.1 leaves $PSScriptRoot empty in parameter defaults, so the paths are worked out here.
$vars = @{}
foreach ($kv in ($Env -split ';')) { if ($kv -match '^([^=]+)=(.*)$') { $vars[$Matches[1]] = $Matches[2] } }
$root = (Resolve-Path (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) '..')).Path
if (-not $Profile) { $Profile = Join-Path $root 'build\profile-kar-test' }
if (-not $Out) { $Out = Join-Path $env:TEMP 'kar_race_test' }
if (-not (Test-Path $Profile)) { Write-Output "profile $Profile not found: not starting"; exit 2 }
$exe = if ($Exe) { $Exe } else { Join-Path $root 'build\kar458752\kar.exe' }
if (Get-Process | Where-Object { $_.ProcessName -match 'WardogsClient|^kar$' }) {
    Write-Output 'another game or kar.exe is running: not starting'
    exit 2
}
New-Item -ItemType Directory -Force $Out | Out-Null
$dirs = @()
for ($k = 1; $k -le $Runs; $k++) {
    $d = Join-Path $Out "run$k"
    Remove-Item -Recurse -Force $d -ErrorAction SilentlyContinue
    $env:TOTK_PROFILE_DIR = (Resolve-Path $Profile).Path
    $env:TOTK_INPUT_SCRIPT = (Resolve-Path $Script).Path
    $env:TOTK_AUDIO = 'none'
    $env:TOTK_SHOTS = $Shots
    $env:TOTK_SHOTS_DIR = $d
    foreach ($key in $vars.Keys) { Set-Item "env:$key" $vars[$key] }
    $p = Start-Process -FilePath $exe -WorkingDirectory (Split-Path $exe) -RedirectStandardError "$d.err" -PassThru
    if ($Affinity) { Start-Sleep $AffinityDelay; try { $p.ProcessorAffinity = [IntPtr]$Affinity } catch { Write-Output "affinity not set: $_" } }
    for ($i = 0; $i -lt $MaxMinutes * 12; $i++) {
        Start-Sleep 5
        if ($p.HasExited) { Write-Output "run $k exited early (see $d.err)"; break }
        if ((Get-ChildItem $d -Filter *.png -ErrorAction SilentlyContinue).Count -ge $ShotCount) { break }
    }
    if (-not $p.HasExited) { $p.CloseMainWindow() | Out-Null; $p.WaitForExit(90000) | Out-Null }
    foreach ($key in $vars.Keys) { Remove-Item "env:$key" -ErrorAction SilentlyContinue }
    $fatal = Select-String -Path "$d.err" -Pattern '^FATAL:' -CaseSensitive -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($fatal) { Write-Output "run ${k}: $($fatal.Line)" }
    $dirs += $d
}
& (Join-Path $root '.venv\Scripts\python.exe') (Join-Path $root 'tools\kar_shot_check.py') @dirs | Where-Object { $_ -notmatch '^  ' }
exit $LASTEXITCODE
