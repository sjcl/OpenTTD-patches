param([Parameter(Mandatory=$true)][string]$Binary, [Parameter(Mandatory=$true)][string]$Label)
$taskRun = $PSScriptRoot
$taskStart = [Diagnostics.Stopwatch]::StartNew()
$taskProcess = Start-Process -FilePath (Join-Path $taskRun $Binary) -ArgumentList '-c openttd.cfg -g input.sav -v null:until_exit -s null -m null -x' -WorkingDirectory $taskRun -WindowStyle Hidden -RedirectStandardOutput (Join-Path $taskRun "$Label-stdout.txt") -RedirectStandardError (Join-Path $taskRun "$Label-stderr.txt") -PassThru
$taskProcess.Id | Set-Content (Join-Path $taskRun "$Label.pid")
$taskPeakPrivate = 0L
$taskPeakWorking = 0L
$taskCpuSeconds = 0.0
while (!$taskProcess.HasExited) {
    $taskProcess.Refresh()
    $taskPeakPrivate = [Math]::Max($taskPeakPrivate, $taskProcess.PrivateMemorySize64)
    $taskPeakWorking = [Math]::Max($taskPeakWorking, $taskProcess.PeakWorkingSet64)
    $taskCpuSeconds = [Math]::Max($taskCpuSeconds, $taskProcess.TotalProcessorTime.TotalSeconds)
    Start-Sleep -Milliseconds 100
}
$taskStart.Stop()
$taskCpuSeconds = [Math]::Max($taskCpuSeconds, $taskProcess.TotalProcessorTime.TotalSeconds)
[ordered]@{exit_code=$taskProcess.ExitCode; wall_seconds=$taskStart.Elapsed.TotalSeconds; cpu_seconds=$taskCpuSeconds; peak_working_set=$taskPeakWorking; sampled_peak_private=$taskPeakPrivate; sample_ms=100} | ConvertTo-Json | Set-Content (Join-Path $taskRun "$Label-process.json")
Get-Content (Join-Path $taskRun "$Label-process.json")
