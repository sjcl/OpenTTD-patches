$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskLog = Join-Path $taskRoot 'wpr-buffered-recorder.log'
$taskWpr = Join-Path $env:WINDIR 'System32/wpr.exe'
$taskIdentity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (!$taskIdentity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Administrator token required' }
if (Test-Path -LiteralPath (Join-Path $taskRoot 'legacy/wpr-go.txt')) { throw 'Legacy has already started; do not interrupt its recorder' }
$taskOld = Get-CimInstance Win32_Process -Filter 'ProcessId=47124'
if ($null -ne $taskOld) {
    if ($taskOld.CommandLine -notlike '*build-wpr-0b8a21c0*record_legacy_file.ps1*') { throw 'Unexpected recorder process' }
    Stop-Process -Id 47124 -Force
}
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
foreach ($taskName in @('legacy','hyperpath')) {
    $taskDir=Join-Path $taskRoot $taskName
    $taskStarted=$false
    try {
        if ($taskName -eq 'hyperpath') { Wait-TaskFile (Join-Path $taskRoot 'hyperpath-buffered-stage-ready.txt') 2700 }
        Wait-TaskFile (Join-Path $taskDir 'wpr-ready.txt') 2700
        $taskProfile=(Join-Path $taskRoot 'cpu-file-buffered.wprp')+'!CPU'
        Invoke-TaskWpr -WprArgs @('-start',$taskProfile,'-filemode','-recordtempto',(Join-Path $taskRoot 'wpr-filemode-temp'))
        $taskStarted=$true
        Start-Sleep -Seconds 1
        Invoke-TaskWpr -WprArgs @('-marker',"$taskName-ready",'-flush')
        Invoke-TaskWpr -WprArgs @('-marker',"$taskName-begin",'-flush')
        [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-go.txt'),(Get-Date -Format o))
        Wait-TaskFile (Join-Path $taskDir 'wpr-finished.txt') 600
        Invoke-TaskWpr -WprArgs @('-marker',"$taskName-end",'-flush')
        Invoke-TaskWpr -WprArgs @('-stop',(Join-Path $taskDir 'cpu.etl'))
        $taskStarted=$false
        [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-saved.txt'),(Get-Date -Format o))
    } catch {
        if ($taskStarted) { & $taskWpr -stop (Join-Path $taskDir 'cpu-incomplete.etl') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8 }
        [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-error.txt'),$_.ToString())
        $_ | Out-File -LiteralPath $taskLog -Append -Encoding utf8
        exit 1
    }
}
[IO.File]::WriteAllText((Join-Path $taskRoot 'wpr-buffered-recorder-complete.txt'),(Get-Date -Format o))
