$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
$taskLog=Join-Path $taskRoot 'recorder-resume.log'
$taskWpr=Join-Path $env:WINDIR 'System32/wpr.exe'
$taskToolkit=Join-Path (Split-Path $taskRoot -Parent) 'build-wpr-9ab0228d/tools/wpt/Windows Kits/10/Windows Performance Toolkit'
$taskXperf=Join-Path $taskToolkit 'xperf.exe'
$taskProfile=Join-Path $taskRoot 'cpu-minimal.wprp'
$taskProfileHash=(Get-FileHash -LiteralPath $taskProfile -Algorithm SHA256).Hash.ToLowerInvariant()
if ($taskProfileHash -ne 'e0a89dab28d5379469aa3ad23f65e8c87256014951065dae14b9429998251a82') { throw 'Unexpected capture profile' }
[IO.File]::WriteAllText((Join-Path $taskRoot 'recorder-resume-pid.txt'),[string]$PID)
function Wait-TaskFile([string]$Path,[int]$Seconds) {
    $taskUntil=(Get-Date).AddSeconds($Seconds)
    while (!(Test-Path -LiteralPath $Path)) {
        if ((Get-Date) -gt $taskUntil) { throw "Timed out waiting for $Path" }
        Start-Sleep -Milliseconds 100
    }
}
function Invoke-TaskWpr([string[]]$WprArgs) {
    "$(Get-Date -Format o) wpr $($WprArgs -join ' ')" | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    $taskPreviousPreference=$ErrorActionPreference
    try {
        $ErrorActionPreference='Continue'
        & $taskWpr @WprArgs 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
        $taskExit=$LASTEXITCODE
    } finally { $ErrorActionPreference=$taskPreviousPreference }
    if ($taskExit -ne 0) { throw "WPR exit code $taskExit; see recorder-resume.log" }
}
New-Item -ItemType Directory -Force -Path (Join-Path $taskRoot 'wpr-temp-resume') | Out-Null
& $taskWpr -profint 2>&1 | Out-File -LiteralPath (Join-Path $taskRoot 'profile-interval-resume.txt') -Encoding utf8
if ($LASTEXITCODE -ne 0 -or (Get-Content -LiteralPath (Join-Path $taskRoot 'profile-interval-resume.txt') -Raw) -notmatch '10000') { throw 'Expected 1ms sample interval' }
$taskUntil=(Get-Date).AddHours(2)
while ((Get-Date) -lt $taskUntil -and !(Test-Path -LiteralPath (Join-Path $taskRoot 'recorder-resume-stop.txt'))) {
    foreach ($taskJob in @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'sampling-jobs-resume') -Directory | Sort-Object Name)) {
        if (!(Test-Path -LiteralPath (Join-Path $taskJob.FullName 'job.json')) -or (Test-Path -LiteralPath (Join-Path $taskJob.FullName 'done.txt'))) { continue }
        $taskSpec=Get-Content -LiteralPath (Join-Path $taskJob.FullName 'job.json') -Raw | ConvertFrom-Json
        if ($taskSpec.mode -notin @('pilot','legacy','hyperpath') -or $taskSpec.kind -notin @('pilot','map')) { throw 'Unexpected job' }
        $taskDir=[IO.Path]::GetFullPath((Join-Path $taskRoot $taskSpec.directory))
        if (!$taskDir.StartsWith($taskRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Outside task root' }
        if ($taskSpec.kind -eq 'pilot') { New-Item -ItemType Directory -Path $taskDir | Out-Null }
        elseif (!(Test-Path -LiteralPath $taskDir -PathType Container)) { throw 'Runner must create the map directory before publishing its job' }
        $taskMode=$taskSpec.mode;$taskStarted=$false
        try {
            if ($taskSpec.kind -eq 'map') { Wait-TaskFile (Join-Path $taskDir 'wpr-ready.txt') 2700 }
            Invoke-TaskWpr -WprArgs @('-start',($taskProfile+'!CPU'),'-filemode','-recordtempto',(Join-Path $taskRoot 'wpr-temp-resume'))
            $taskStarted=$true
            Start-Sleep -Seconds 1
            Invoke-TaskWpr -WprArgs @('-marker',"$taskMode-begin",'-flush')
            if ($taskSpec.kind -eq 'pilot') {
                $taskTimer=[Diagnostics.Stopwatch]::StartNew()
                while ($taskTimer.Elapsed.TotalSeconds -lt 3) { [Math]::Sqrt(12345) | Out-Null }
            } else {
                [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-go.txt'),(Get-Date -Format o))
                Wait-TaskFile (Join-Path $taskDir 'wpr-finished.txt') 780
            }
            Invoke-TaskWpr -WprArgs @('-marker',"$taskMode-end",'-flush')
            Invoke-TaskWpr -WprArgs @('-stop',(Join-Path $taskDir 'cpu.etl'))
            $taskStarted=$false
            & $taskXperf -i (Join-Path $taskDir 'cpu.etl') -o (Join-Path $taskDir 'recorder-trace-stats.txt') -a tracestats -timespan 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
            $taskStatsExit=$LASTEXITCODE
            & $taskXperf -i (Join-Path $taskDir 'cpu.etl') -a marks -summary -ao (Join-Path $taskDir 'recorder-markers.txt') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
            $taskMarksExit=$LASTEXITCODE
            $taskStats=Get-Content -LiteralPath (Join-Path $taskDir 'recorder-trace-stats.txt') -Raw
            $taskMarks=Get-Content -LiteralPath (Join-Path $taskDir 'recorder-markers.txt') -Raw
            $taskBegin=[regex]::Match($taskMarks,"(?m)^\s*Mark,\s*(\d+),\s*$taskMode-begin\s*$")
            $taskEnd=[regex]::Match($taskMarks,"(?m)^\s*Mark,\s*(\d+),\s*$taskMode-end\s*$")
            $taskValid=($taskStatsExit -eq 0 -and $taskMarksExit -eq 0 -and $taskStats -match 'Total # Lost Events\s+: 0' -and $taskStats -match 'Total # Lost Buffers\s+: 0' -and $taskBegin.Success -and $taskEnd.Success)
            if ($taskValid) { $taskValid=([long]$taskBegin.Groups[1].Value -lt [long]$taskEnd.Groups[1].Value) }
            @{valid=$taskValid;stats_exit=$taskStatsExit;marks_exit=$taskMarksExit;kind=$taskSpec.kind;directory=$taskSpec.directory;profile_sha256=$taskProfileHash} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskJob.FullName 'quality.json') -Encoding utf8
            if ($taskSpec.kind -eq 'map') { [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-saved.txt'),(Get-Date -Format o)) }
            [IO.File]::WriteAllText((Join-Path $taskJob.FullName 'done.txt'),(Get-Date -Format o))
            if (!$taskValid) { throw 'Trace quality failed; do not continue the queue' }
        } catch {
            if ($taskStarted) { & $taskWpr -stop (Join-Path $taskDir 'cpu-incomplete.etl') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8 }
            [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-error.txt'),$_.ToString())
            $_ | Out-File -LiteralPath $taskLog -Append -Encoding utf8
            exit 1
        }
    }
    Start-Sleep -Milliseconds 200
}
[IO.File]::WriteAllText((Join-Path $taskRoot 'recorder-resume-complete.txt'),(Get-Date -Format o))
