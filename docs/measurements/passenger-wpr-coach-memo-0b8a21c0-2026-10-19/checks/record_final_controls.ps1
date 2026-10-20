$ErrorActionPreference='Stop'
$taskRoot=$PSScriptRoot
$taskLog=Join-Path $taskRoot 'wpr-final-controls.log'
$taskWpr=Join-Path $env:WINDIR 'System32/wpr.exe'
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
Wait-TaskFile (Join-Path $taskRoot 'final-captures-stage-start.txt') 7200
foreach ($taskRun in @(@{Label='target-legacy';Path='legacy';Mode='legacy'},@{Label='control-legacy';Path='control-97cdc0ad/legacy';Mode='legacy'},@{Label='control-hyperpath';Path='control-97cdc0ad/hyperpath';Mode='hyperpath'})) {
    $taskLabel=$taskRun.Label;$taskDir=Join-Path $taskRoot $taskRun.Path;$taskMode=$taskRun.Mode;$taskStarted=$false
    try {
        Wait-TaskFile (Join-Path $taskRoot ($taskLabel+'-stage-ready.txt')) 2700
        Wait-TaskFile (Join-Path $taskDir 'wpr-ready.txt') 2700
        Invoke-TaskWpr -WprArgs @('-start',((Join-Path $taskRoot 'cpu-file-buffered.wprp')+'!CPU'),'-filemode','-recordtempto',(Join-Path $taskRoot 'wpr-filemode-temp'))
        $taskStarted=$true
        Start-Sleep -Seconds 1
        Invoke-TaskWpr -WprArgs @('-marker',"$taskMode-ready",'-flush')
        Invoke-TaskWpr -WprArgs @('-marker',"$taskMode-begin",'-flush')
        [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-go.txt'),(Get-Date -Format o))
        Wait-TaskFile (Join-Path $taskDir 'wpr-finished.txt') 600
        Invoke-TaskWpr -WprArgs @('-marker',"$taskMode-end",'-flush')
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
[IO.File]::WriteAllText((Join-Path $taskRoot 'wpr-final-controls-complete.txt'),(Get-Date -Format o))
