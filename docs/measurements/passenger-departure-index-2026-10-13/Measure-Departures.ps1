param(
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$Label,
    [ValidateSet('legacy','periodic','off')][string]$Backend = 'periodic',
    [switch]$Original,
    [string]$InputSave = 'input.sav',
    [switch]$Resume,
    [int]$Ticks = 0,
    [switch]$DisablePolicies,
    [switch]$DisableDepartures
)
$ErrorActionPreference = 'Stop'
$taskRun = (Resolve-Path -LiteralPath $RunDirectory).Path
$taskForwardRun = $taskRun.Replace('\','/')
$taskScripts = Join-Path $taskRun 'scripts'
New-Item -ItemType Directory -Path $taskScripts -Force | Out-Null
$taskJourney = if ($Backend -eq 'off') { 'false' } else { 'true' }
$taskPeriodic = if ($Backend -eq 'periodic') { 'true' } else { 'false' }
$taskPeriodicSetting = if ($Original) { '' } else { "setting linkgraph.passenger_journey_periodic $taskPeriodic" }
$taskSettings = if ($Resume) { '' } else { @"
setting linkgraph.distribution_pax 2
setting linkgraph.passenger_od_assignment true
setting linkgraph.passenger_journey_routing $taskJourney
$taskPeriodicSetting
setting linkgraph.recalc_interval 8
setting linkgraph.recalc_time 32
"@ }
$taskStart = @"
script $taskForwardRun/$Label-console.txt
getfulldate
passenger_journey_stats inventory
$taskSettings
$(if ($DisablePolicies) { "passenger_journey_stats cache_off" })
$(if ($DisableDepartures) { "passenger_journey_stats departures_off" } else { "passenger_journey_stats departures_on" })
passenger_journey_stats reset
unpause
schedule on-next-calendar-month $taskForwardRun/scripts/finish.scr
script
"@
$taskFinish = @"
script $taskForwardRun/$Label-console.txt
getfulldate
passenger_journey_stats inventory
passenger_journey_stats total
save $taskForwardRun/$Label-result
script
quit
"@
if ($Ticks -gt 0) { $taskStart = $taskStart.Replace("schedule on-next-calendar-month $taskForwardRun/scripts/finish.scr", '') }
[IO.File]::WriteAllText((Join-Path $taskScripts 'game_start.scr'), (($taskStart -split '\r?\n' | Where-Object { ![string]::IsNullOrWhiteSpace($_) }) -join "`n"))
[IO.File]::WriteAllText((Join-Path $taskScripts 'finish.scr'), $taskFinish)
if ($Ticks -gt 0) {
    [IO.File]::WriteAllText((Join-Path $taskScripts 'passenger_benchmark_end.scr'),
        "script $taskForwardRun/$Label-console.txt`ngetfulldate`npassenger_journey_stats inventory`npassenger_journey_stats total`nsave $taskForwardRun/$Label-result`nscript`n")
}
$taskClock = [Diagnostics.Stopwatch]::StartNew()
$taskDriver = if ($Ticks -gt 0) { "null:ticks=$Ticks" } else { 'null:until_exit' }
$taskProcess = Start-Process -FilePath (Join-Path $taskRun $Binary) -ArgumentList @('-c','openttd.cfg','-g',('"' + $InputSave + '"'),'-v',$taskDriver,'-s','null','-m','null','-x') -WorkingDirectory $taskRun -WindowStyle Hidden -RedirectStandardOutput (Join-Path $taskRun "$Label-stdout.txt") -RedirectStandardError (Join-Path $taskRun "$Label-stderr.txt") -PassThru
$taskProcess.Id | Set-Content (Join-Path $taskRun "$Label.pid")
$null = $taskProcess.Handle # Keep the native exit handle open before the child can exit.
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
    backend = $Backend; exit_code = $taskExit; wall_seconds = $taskClock.Elapsed.TotalSeconds
    cpu_seconds = $taskCpu; peak_working_set = $taskPeakWorking; sampled_peak_private = $taskPeakPrivate
    input_save = $InputSave; resumed = [bool]$Resume
    requested_driver_loops = $Ticks; policy_cache_disabled = [bool]$DisablePolicies; departure_index_disabled = [bool]$DisableDepartures
    input_sha256 = (Get-FileHash -LiteralPath (Join-Path $taskRun $InputSave)).Hash
    binary_sha256 = (Get-FileHash -LiteralPath (Join-Path $taskRun $Binary)).Hash
    sample_ms = 100
} | ConvertTo-Json | Set-Content (Join-Path $taskRun "$Label-process.json")
Get-Content (Join-Path $taskRun "$Label-process.json")
if ($null -ne $taskExit -and $taskExit -ne 0) { throw "Benchmark $Label failed: $taskExit" }
