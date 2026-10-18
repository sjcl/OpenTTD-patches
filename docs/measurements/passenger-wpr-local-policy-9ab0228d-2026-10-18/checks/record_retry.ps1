$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskDir = Join-Path $taskRoot 'hyperpath'
$taskLog = Join-Path $taskRoot 'wpr-retry-recorder.log'
$taskWpr = Join-Path $env:WINDIR 'System32/wpr.exe'
$taskIdentity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (!$taskIdentity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'WPR requires an administrator token' }
function Wait-TaskFile([string]$Path, [int]$Seconds) {
    $taskDeadline = (Get-Date).AddSeconds($Seconds)
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
$taskStarted = $false
try {
    Wait-TaskFile (Join-Path $taskDir 'wpr-ready.txt') 2700
    Invoke-TaskWpr -WprArgs @('-start', 'CPU', '-filemode', '-recordtempto', (Join-Path $taskRoot 'wpr-filemode-temp'))
    $taskStarted = $true
    # Allow provider startup before the real begin marker. The game is still gated;
    # this one-second preflight is outside the measured marker interval.
    Start-Sleep -Seconds 1
    Invoke-TaskWpr -WprArgs @('-marker', 'hyperpath-ready', '-flush')
    Invoke-TaskWpr -WprArgs @('-marker', 'hyperpath-begin', '-flush')
    [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-go.txt'), (Get-Date -Format o))
    Wait-TaskFile (Join-Path $taskDir 'wpr-finished.txt') 600
    Invoke-TaskWpr -WprArgs @('-marker', 'hyperpath-end', '-flush')
    Invoke-TaskWpr -WprArgs @('-stop', (Join-Path $taskDir 'cpu.etl'))
    $taskStarted = $false
    [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-saved.txt'), (Get-Date -Format o))
    [IO.File]::WriteAllText((Join-Path $taskRoot 'wpr-retry-recorder-complete.txt'), (Get-Date -Format o))
} catch {
    if ($taskStarted) { & $taskWpr -stop (Join-Path $taskDir 'cpu-incomplete.etl') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8 }
    if (Test-Path -LiteralPath $taskDir) { [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-error.txt'), $_.ToString()) }
    $_ | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    exit 1
}
