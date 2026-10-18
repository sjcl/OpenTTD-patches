# Stop only our old recorder, which is waiting for the next file after its completed trace.
$ErrorActionPreference='Stop'
$taskOld = Get-CimInstance Win32_Process -Filter 'ProcessId=106032'
if ($taskOld) {
    if ($taskOld.Name -ne 'powershell.exe' -or $taskOld.CommandLine -notmatch 'record_rebuild\.ps1') {
        throw 'The old recorder PID no longer identifies this measurement helper'
    }
    $taskLegacy=Join-Path $PSScriptRoot 'rebuild/baseline/rejected-build-overlap-legacy/wpr-saved.txt'
    if (!(Test-Path -LiteralPath $taskLegacy)) { throw 'The old recording has not stopped' }
    Stop-Process -Id $taskOld.ProcessId -ErrorAction Stop
}
& (Join-Path $PSScriptRoot 'record_rebuild.ps1')
exit $LASTEXITCODE
