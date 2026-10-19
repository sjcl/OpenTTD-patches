$ErrorActionPreference = 'Stop'
$taskRoot = $PSScriptRoot
$taskWpr = Join-Path $env:WINDIR 'System32/wpr.exe'
$taskLog = Join-Path $taskRoot 'recorder.log'
$taskDeadline = (Get-Date).AddMinutes(30)
$taskStarted = $false
function Invoke-TaskWpr([string[]]$WprArgs) {
    "$(Get-Date -Format o) wpr $($WprArgs -join ' ')" | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    & $taskWpr @WprArgs 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    if ($LASTEXITCODE -ne 0) { throw "WPR exit code $LASTEXITCODE" }
}
try {
    while ((Get-Date) -lt $taskDeadline -and !(Test-Path -LiteralPath (Join-Path $taskRoot 'recorder-stop.txt'))) {
        $taskJobs = @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'runs') -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^wpr-(baseline|pgo)-[1-4]$' } | Sort-Object CreationTime)
        foreach ($taskJob in $taskJobs) {
            $taskDir = $taskJob.FullName
            if (!(Test-Path -LiteralPath (Join-Path $taskDir 'wpr-ready.txt')) -or
                (Test-Path -LiteralPath (Join-Path $taskDir 'wpr-saved.txt'))) { continue }
            Invoke-TaskWpr -WprArgs @('-start', "$(Join-Path $taskRoot 'cpu-sampled.wprp')!CPU", '-filemode')
            $taskStarted = $true
            Start-Sleep -Seconds 1
            Invoke-TaskWpr -WprArgs @('-marker', 'simulation-begin', '-flush')
            [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-go.txt'), (Get-Date -Format o))
            $taskFinishDeadline = (Get-Date).AddSeconds(600)
            while (!(Test-Path -LiteralPath (Join-Path $taskDir 'wpr-finished.txt'))) {
                if ((Get-Date) -gt $taskFinishDeadline) { throw 'Simulation gate timed out' }
                Start-Sleep -Milliseconds 100
            }
            Invoke-TaskWpr -WprArgs @('-marker', 'simulation-end', '-flush')
            Invoke-TaskWpr -WprArgs @('-stop', (Join-Path $taskDir 'cpu.etl'))
            $taskStarted = $false
            [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-saved.txt'), (Get-Date -Format o))
        }
        Start-Sleep -Seconds 1
    }
    [IO.File]::WriteAllText((Join-Path $taskRoot 'recorder-complete.txt'), (Get-Date -Format o))
} catch {
    if ($taskStarted) { & $taskWpr -stop (Join-Path $taskDir 'cpu-incomplete.etl') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8 }
    if (Test-Path -LiteralPath $taskDir) { [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-error.txt'), $_.ToString()) }
    $_ | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    exit 1
}
