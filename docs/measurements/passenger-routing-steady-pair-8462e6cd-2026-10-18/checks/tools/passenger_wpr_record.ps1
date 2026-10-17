param(
    [Parameter(Mandatory=$true)][string]$Build,
    [ValidateSet('legacy', 'hyperpath')][string[]]$Mode = @('legacy', 'hyperpath')
)
$ErrorActionPreference = 'Stop'
$taskRoot = (Resolve-Path -LiteralPath $Build).Path
$taskIdentity = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (!$taskIdentity.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'WPR requires an administrator token. The game must remain unelevated.'
}
$taskWpr = Join-Path $env:WINDIR 'System32/wpr.exe'
$taskLog = Join-Path $taskRoot 'wpr-recorder.log'
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
foreach ($taskName in $Mode) {
    $taskDir = Join-Path $taskRoot $taskName
    $taskStarted = $false
    try {
        Wait-TaskFile (Join-Path $taskDir 'wpr-ready.txt') 2700
        Invoke-TaskWpr -WprArgs @('-start', 'CPU')
        $taskStarted = $true
        Invoke-TaskWpr -WprArgs @('-marker', "$taskName-begin", '-flush')
        [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-go.txt'), (Get-Date -Format o))
        Wait-TaskFile (Join-Path $taskDir 'wpr-finished.txt') 600
        Invoke-TaskWpr -WprArgs @('-marker', "$taskName-end", '-flush')
        Invoke-TaskWpr -WprArgs @('-stop', (Join-Path $taskDir 'cpu.etl'))
        $taskStarted = $false
        [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-saved.txt'), (Get-Date -Format o))
    } catch {
        if ($taskStarted) {
            # Stop only the recording successfully started by this helper.
            & $taskWpr -stop (Join-Path $taskDir 'cpu-incomplete.etl') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
        }
        if (Test-Path -LiteralPath $taskDir) {
            [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-error.txt'), $_.ToString())
        }
        $_ | Out-File -LiteralPath $taskLog -Append -Encoding utf8
        exit 1
    }
}
[IO.File]::WriteAllText((Join-Path $taskRoot 'wpr-recorder-complete.txt'), (Get-Date -Format o))
