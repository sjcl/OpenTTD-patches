param([Parameter(Mandatory=$true)][string]$RunDirectory, [Parameter(Mandatory=$true)][string]$Binary,
      [Parameter(Mandatory=$true)][string]$InputSave, [int]$Count = 128)
$ErrorActionPreference = 'Stop'
$taskRun = [IO.Path]::GetFullPath($RunDirectory)
New-Item -ItemType Directory -Path (Join-Path $taskRun 'scripts') -Force | Out-Null
$taskLog = (Join-Path $taskRun 'quality-console.txt').Replace('\','/')
[IO.File]::WriteAllText((Join-Path $taskRun 'scripts/game_start.scr'), "script $taskLog`ngetfulldate`npassenger_periodic_compare sample $Count`ngetfulldate`nscript`nquit`n")
$taskProcess = Start-Process -FilePath ([IO.Path]::GetFullPath($Binary)) -ArgumentList @('-c','openttd.cfg','-g',('"' + [IO.Path]::GetFullPath($InputSave) + '"'),'-v','null:until_exit','-s','null','-m','null','-x') -WorkingDirectory $taskRun -WindowStyle Hidden -RedirectStandardOutput (Join-Path $taskRun 'quality-stdout.txt') -RedirectStandardError (Join-Path $taskRun 'quality-stderr.txt') -PassThru
$null = $taskProcess.Handle
$taskProcess.WaitForExit()
if ($taskProcess.ExitCode -ne 0) { throw "Quality snapshot failed: $($taskProcess.ExitCode)" }
Get-Content -LiteralPath $taskLog | Select-String 'Timed pruning quality:|State ticks:'
