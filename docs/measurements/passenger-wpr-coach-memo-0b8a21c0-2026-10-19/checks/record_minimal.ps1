$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
$taskLog=Join-Path $taskRoot 'wpr-minimal-recorder.log'
$taskWpr=Join-Path $env:WINDIR 'System32/wpr.exe'
$taskXperf=Join-Path (Split-Path $taskRoot -Parent) 'build-wpr-9ab0228d/tools/wpt/Windows Kits/10/Windows Performance Toolkit/xperf.exe'
# Stop only the owned, still-waiting obsolete recorder.
$taskOld=Get-CimInstance Win32_Process | Where-Object { $_.Name -eq 'powershell.exe' -and $_.CommandLine -like '*build-wpr-0b8a21c0*record_final_controls.ps1*' }
foreach ($taskProcess in $taskOld) { Stop-Process -Id $taskProcess.ProcessId -Force }
function Wait-TaskFile([string]$Path,[int]$Seconds) {
    $taskDeadline=(Get-Date).AddSeconds($Seconds)
    while (!(Test-Path -LiteralPath $Path)) {
        if ((Get-Date) -gt $taskDeadline) { throw "Timed out waiting for $Path" }
        Start-Sleep -Milliseconds 100
    }
}
function Invoke-TaskWpr([string[]]$WprArgs) {
    "$(Get-Date -Format o) wpr $($WprArgs -join ' ')" | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    & $taskWpr @WprArgs 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "WPR exit code $LASTEXITCODE" }
}
$taskDeadline=(Get-Date).AddHours(3)
while ((Get-Date) -lt $taskDeadline -and !(Test-Path -LiteralPath (Join-Path $taskRoot 'minimal-recorder-stop.txt'))) {
    foreach ($taskJob in @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'sampling-jobs') -Directory | Sort-Object CreationTime)) {
        if (!(Test-Path -LiteralPath (Join-Path $taskJob.FullName 'job.json')) -or (Test-Path -LiteralPath (Join-Path $taskJob.FullName 'done.txt'))) { continue }
        $taskSpec=Get-Content -LiteralPath (Join-Path $taskJob.FullName 'job.json') -Raw | ConvertFrom-Json
        if ($taskSpec.mode -notin @('pilot','legacy','hyperpath')) { throw 'Unexpected mode' }
        $taskDir=[IO.Path]::GetFullPath((Join-Path $taskRoot $taskSpec.directory))
        if (!$taskDir.StartsWith($taskRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Outside task root' }
        New-Item -ItemType Directory -Force -Path $taskDir | Out-Null
        $taskMode=$taskSpec.mode;$taskStarted=$false
        try {
            if ($taskSpec.kind -ne 'pilot') { Wait-TaskFile (Join-Path $taskDir 'wpr-ready.txt') 2700 }
            Invoke-TaskWpr -WprArgs @('-start',((Join-Path $taskRoot 'cpu-minimal.wprp')+'!CPU'),'-filemode','-recordtempto',(Join-Path $taskRoot 'wpr-filemode-temp'))
            $taskStarted=$true
            Start-Sleep -Seconds 1
            Invoke-TaskWpr -WprArgs @('-marker',"$taskMode-begin",'-flush')
            if ($taskSpec.kind -eq 'pilot') {
                $taskTimer=[Diagnostics.Stopwatch]::StartNew()
                while ($taskTimer.Elapsed.TotalSeconds -lt 3) { [Math]::Sqrt(12345) | Out-Null }
            } else {
                [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-go.txt'),(Get-Date -Format o))
                Wait-TaskFile (Join-Path $taskDir 'wpr-finished.txt') 600
            }
            Invoke-TaskWpr -WprArgs @('-marker',"$taskMode-end",'-flush')
            Invoke-TaskWpr -WprArgs @('-stop',(Join-Path $taskDir 'cpu.etl'))
            $taskStarted=$false
            & $taskXperf -i (Join-Path $taskDir 'cpu.etl') -o (Join-Path $taskDir 'recorder-trace-stats.txt') -a tracestats -timespan 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
            $taskStatsExit=$LASTEXITCODE
            & $taskXperf -i (Join-Path $taskDir 'cpu.etl') -a marks -summary -ao (Join-Path $taskDir 'recorder-markers.txt') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
            $taskMarksExit=$LASTEXITCODE
            $taskStats=Get-Content -LiteralPath (Join-Path $taskDir 'recorder-trace-stats.txt') -Raw -ErrorAction SilentlyContinue
            $taskMarks=Get-Content -LiteralPath (Join-Path $taskDir 'recorder-markers.txt') -Raw -ErrorAction SilentlyContinue
            $taskValid=($taskStatsExit -eq 0 -and $taskMarksExit -eq 0 -and $taskStats -match 'Total # Lost Events\s+: 0' -and $taskStats -match 'Total # Lost Buffers\s+: 0' -and $taskMarks -match "$taskMode-begin" -and $taskMarks -match "$taskMode-end")
            @{valid=$taskValid;stats_exit=$taskStatsExit;marks_exit=$taskMarksExit;kind=$taskSpec.kind;directory=$taskSpec.directory} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskJob.FullName 'quality.json') -Encoding utf8
            if ($taskSpec.kind -ne 'pilot') { [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-saved.txt'),(Get-Date -Format o)) }
            [IO.File]::WriteAllText((Join-Path $taskJob.FullName 'done.txt'),(Get-Date -Format o))
        } catch {
            if ($taskStarted) { & $taskWpr -stop (Join-Path $taskDir 'cpu-incomplete.etl') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8 }
            [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-error.txt'),$_.ToString())
            $_ | Out-File -LiteralPath $taskLog -Append -Encoding utf8
            exit 1
        }
    }
    Start-Sleep -Milliseconds 200
}
[IO.File]::WriteAllText((Join-Path $taskRoot 'minimal-recorder-complete.txt'),(Get-Date -Format o))
