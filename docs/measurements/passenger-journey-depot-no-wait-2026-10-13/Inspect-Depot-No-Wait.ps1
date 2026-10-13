param(
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$Label,
    [Parameter(Mandatory=$true)][string]$InputSave
)
$ErrorActionPreference = 'Stop'
$taskRun = (Resolve-Path -LiteralPath $RunDirectory).Path
$taskLog = Join-Path $taskRun "$Label-audit.txt"
if (Test-Path -LiteralPath $taskLog) { Remove-Item -LiteralPath $taskLog }
$taskStart = "script $($taskLog.Replace('\','/'))`ngetfulldate`npassenger_journey_stats total`npassenger_service_inspect depots`nscript`nquit`n"
[IO.File]::WriteAllText((Join-Path $taskRun 'scripts/game_start.scr'), $taskStart)
$taskProcess = Start-Process -FilePath (Join-Path $taskRun $Binary) -ArgumentList @('-c','openttd.cfg','-g',('"' + $InputSave + '"'),'-v','null:until_exit','-s','null','-m','null','-x') -WorkingDirectory $taskRun -WindowStyle Hidden -RedirectStandardOutput (Join-Path $taskRun "$Label-audit-stdout.txt") -RedirectStandardError (Join-Path $taskRun "$Label-audit-stderr.txt") -PassThru
$taskProcess.WaitForExit()
if ($taskProcess.ExitCode -ne 0) { throw "Depot audit failed: $($taskProcess.ExitCode)" }
Get-Content -LiteralPath $taskLog | Select-String 'Periodic operating events|State ticks:|Timed depot transport:'
