param([Parameter(Mandatory=$true)][string]$Build)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $Build).Path
$taskScript = (Resolve-Path -LiteralPath (Join-Path $taskRoot '../utils/passenger_wpr_record.ps1')).Path
$taskPreviousPid = [int][IO.File]::ReadAllText((Join-Path $taskRoot 'wpr-recorder-pid.txt'))
$taskPrevious = Get-CimInstance Win32_Process -Filter "ProcessId=$taskPreviousPid"
if ($null -ne $taskPrevious) {
    if ($taskPrevious.Name -ne 'powershell.exe' -or !$taskPrevious.CommandLine.Contains($taskRoot) -or !$taskPrevious.CommandLine.Contains($taskScript)) {
        throw 'The previous recorder identity does not match the helper started for this measurement.'
    }
    $taskActiveChildren = Get-CimInstance Win32_Process -Filter "ParentProcessId=$taskPreviousPid AND Name='wpr.exe'"
    if ($null -ne $taskActiveChildren) { throw 'The previous recorder still has an active WPR command.' }
    Stop-Process -Id $taskPreviousPid -Force
}
Move-Item -LiteralPath (Join-Path $taskRoot 'wpr-recorder.log') -Destination (Join-Path $taskRoot 'wpr-recorder-contaminated.log')
Move-Item -LiteralPath (Join-Path $taskRoot 'wpr-recorder-pid.txt') -Destination (Join-Path $taskRoot 'wpr-recorder-contaminated-pid.txt')
& $taskScript -Build $taskRoot
exit $LASTEXITCODE
