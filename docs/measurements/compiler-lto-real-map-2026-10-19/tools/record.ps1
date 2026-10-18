$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskWpr = Join-Path $env:WINDIR 'System32/wpr.exe'
$taskLog = Join-Path $taskRoot 'recorder.log'
$taskNames = @('wpr-msvc-1', 'wpr-clang-1', 'wpr-clang-2', 'wpr-msvc-2')
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
    foreach ($taskName in $taskNames) {
        $taskDir = Join-Path $taskRoot "runs/$taskName"
        Wait-TaskFile (Join-Path $taskDir 'wpr-ready.txt') 1800
        Invoke-TaskWpr -WprArgs @('-start', "$(Join-Path $taskRoot 'cpu-sampled.wprp')!CPU", '-filemode')
        $taskStarted = $true
        Start-Sleep -Seconds 1
        Invoke-TaskWpr -WprArgs @('-marker', 'simulation-begin', '-flush')
        [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-go.txt'), (Get-Date -Format o))
        Wait-TaskFile (Join-Path $taskDir 'wpr-finished.txt') 600
        Invoke-TaskWpr -WprArgs @('-marker', 'simulation-end', '-flush')
        Invoke-TaskWpr -WprArgs @('-stop', (Join-Path $taskDir 'cpu.etl'))
        $taskStarted = $false
        [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-saved.txt'), (Get-Date -Format o))
    }
    [IO.File]::WriteAllText((Join-Path $taskRoot 'recorder-complete.txt'), (Get-Date -Format o))
} catch {
    if ($taskStarted) { & $taskWpr -stop (Join-Path $taskDir 'cpu-incomplete.etl') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8 }
    if (Test-Path -LiteralPath $taskDir) { [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-error.txt'), $_.ToString()) }
    $_ | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    exit 1
}
