param(
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$Label,
    [int]$Ticks = 1500,
    [string]$InputSave = 'input.sav'
)
$ErrorActionPreference = 'Stop'
$taskRun = (Resolve-Path -LiteralPath $RunDirectory).Path
$taskForwardRun = $taskRun.Replace('\','/')
$taskScripts = Join-Path $taskRun 'scripts'
New-Item -ItemType Directory -Path $taskScripts -Force | Out-Null
$taskSampleCommand = if ($Ticks -gt 2000) { "schedule on-next-calendar-month $taskForwardRun/scripts/sample.scr" } else { '' }
$taskStart = @"
script $taskForwardRun/$Label-console.txt
getfulldate
passenger_journey_stats inventory
setting linkgraph.distribution_pax 2
setting linkgraph.passenger_od_assignment true
setting linkgraph.passenger_journey_routing true
setting linkgraph.passenger_journey_periodic true
setting linkgraph.recalc_interval 8
setting linkgraph.recalc_time 32
passenger_journey_stats reset
unpause
$taskSampleCommand
script
"@
$taskEnd = @"
script $taskForwardRun/$Label-console.txt
getfulldate
passenger_journey_stats inventory
passenger_journey_stats total
save $taskForwardRun/$Label-result
script
"@
[IO.File]::WriteAllText((Join-Path $taskScripts 'game_start.scr'), $taskStart)
[IO.File]::WriteAllText((Join-Path $taskScripts 'passenger_benchmark_end.scr'), $taskEnd)
$taskSample = @"
script $taskForwardRun/$Label-console.txt
getfulldate
passenger_journey_stats inventory
passenger_journey_stats total
schedule on-next-calendar-month $taskForwardRun/scripts/sample.scr
script
"@
[IO.File]::WriteAllText((Join-Path $taskScripts 'sample.scr'), $taskSample)
$taskClock = [Diagnostics.Stopwatch]::StartNew()
$taskProcess = Start-Process -FilePath (Join-Path $taskRun $Binary) -ArgumentList @('-c','openttd.cfg','-g',('"' + $InputSave + '"'),'-v',"null:ticks=$Ticks",'-s','null','-m','null','-x') -WorkingDirectory $taskRun -WindowStyle Hidden -RedirectStandardOutput (Join-Path $taskRun "$Label-stdout.txt") -RedirectStandardError (Join-Path $taskRun "$Label-stderr.txt") -PassThru
$null = $taskProcess.Handle
$taskPeakPrivate = 0L
$taskPeakWorking = 0L
$taskCpu = 0.0
while (!$taskProcess.HasExited) {
    $taskProcess.Refresh()
    $taskPeakPrivate = [Math]::Max($taskPeakPrivate, $taskProcess.PrivateMemorySize64)
    $taskPeakWorking = [Math]::Max($taskPeakWorking, $taskProcess.PeakWorkingSet64)
    $taskCpu = [Math]::Max($taskCpu, $taskProcess.TotalProcessorTime.TotalSeconds)
    Start-Sleep -Milliseconds 100
}
$taskProcess.WaitForExit()
$taskClock.Stop()
$taskExit = $taskProcess.ExitCode
[ordered]@{
    exit_code = $taskExit; wall_seconds = $taskClock.Elapsed.TotalSeconds
    cpu_seconds = $taskCpu; peak_working_set = $taskPeakWorking; sampled_peak_private = $taskPeakPrivate
    requested_driver_loops = $Ticks; input_save = $InputSave
    input_sha256 = (Get-FileHash -LiteralPath (Join-Path $taskRun $InputSave)).Hash
    binary_sha256 = (Get-FileHash -LiteralPath (Join-Path $taskRun $Binary)).Hash
    sample_ms = 100
} | ConvertTo-Json | Set-Content (Join-Path $taskRun "$Label-process.json")
Get-Content (Join-Path $taskRun "$Label-process.json")
if ($taskExit -ne 0) { throw "Benchmark $Label failed: $taskExit" }
