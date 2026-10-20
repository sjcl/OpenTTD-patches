param([Parameter(Mandatory=$true)][string]$Work, [Parameter(Mandatory=$true)][string]$Toolkit)
$ErrorActionPreference='Stop'
$taskRoot=[IO.Path]::GetFullPath($Work)
$taskToolkit=[IO.Path]::GetFullPath($Toolkit)
$taskWpr=Join-Path $env:WINDIR 'System32/wpr.exe'
$taskXperf=Join-Path $taskToolkit 'xperf.exe'
$taskLog=Join-Path $taskRoot 'recorder.log'
$taskTemp=Join-Path $taskRoot 'wpr-temp'
New-Item -ItemType Directory -Path $taskTemp -Force | Out-Null
function Invoke-TaskNative([string]$Exe,[string[]]$Arguments) {
    "$(Get-Date -Format o) $Exe $($Arguments -join ' ')" | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    $taskPreference=$ErrorActionPreference
    $ErrorActionPreference='Continue'
    & $Exe @Arguments 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    $taskCode=$LASTEXITCODE
    $ErrorActionPreference=$taskPreference
    if ($taskCode -ne 0) { throw "Native exit code $taskCode from $Exe" }
}
function Wait-TaskFile([string]$Path,[int]$Seconds) {
    $taskUntil=(Get-Date).AddSeconds($Seconds)
    while (!(Test-Path -LiteralPath $Path)) {
        if ((Get-Date) -gt $taskUntil) { throw "Timed out waiting for $Path" }
        Start-Sleep -Milliseconds 100
    }
}
# Never cancel somebody else's recorder. The dispatcher checks this status first.
$ErrorActionPreference='Continue'
& $taskWpr -status 2>&1 | Out-File -LiteralPath (Join-Path $taskRoot 'wpr-status-before.txt') -Encoding utf8
& $taskWpr -profint 2>&1 | Out-File -LiteralPath (Join-Path $taskRoot 'profile-interval.txt') -Encoding utf8
$ErrorActionPreference='Stop'
$taskUntil=(Get-Date).AddHours(3)
try {
while ((Get-Date) -lt $taskUntil -and !(Test-Path -LiteralPath (Join-Path $taskRoot 'recorder-stop.txt'))) {
    foreach ($taskJob in @(Get-ChildItem -LiteralPath (Join-Path $taskRoot 'jobs') -Directory | Sort-Object Name)) {
        $taskSpecPath=Join-Path $taskJob.FullName 'job.json'
        if (!(Test-Path -LiteralPath $taskSpecPath) -or (Test-Path -LiteralPath (Join-Path $taskJob.FullName 'done.json'))) { continue }
        $taskSpec=Get-Content -LiteralPath $taskSpecPath -Raw | ConvertFrom-Json
        if ($taskSpec.mode -notin @('pilot','legacy','hyperpath')) { throw 'Unexpected mode' }
        $taskDir=[IO.Path]::GetFullPath((Join-Path $taskRoot $taskSpec.directory))
        if (!$taskDir.StartsWith($taskRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Outside task root' }
        if (!(Test-Path -LiteralPath $taskDir)) { throw 'Runner must create the fresh output before job publication' }
        $taskMode=$taskSpec.mode
        $taskStarted=$false
        try {
            if ($taskMode -ne 'pilot') { Wait-TaskFile (Join-Path $taskDir 'wpr-ready.txt') 900 }
            Invoke-TaskNative $taskWpr @('-start',((Join-Path $taskRoot 'cpu.wprp')+'!CPU'),'-filemode','-recordtempto',$taskTemp)
            $taskStarted=$true
            Start-Sleep -Seconds 1
            Invoke-TaskNative $taskWpr @('-marker',"$taskMode-begin",'-flush')
            if ($taskMode -eq 'pilot') {
                $taskWatch=[Diagnostics.Stopwatch]::StartNew()
                while ($taskWatch.Elapsed.TotalSeconds -lt 3) { [Math]::Sqrt(12345) | Out-Null }
            } else {
                [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-go.txt'),(Get-Date -Format o))
                Wait-TaskFile (Join-Path $taskDir 'wpr-finished.txt') 900
            }
            Invoke-TaskNative $taskWpr @('-marker',"$taskMode-end",'-flush')
            Invoke-TaskNative $taskWpr @('-stop',(Join-Path $taskDir 'cpu.etl'))
            $taskStarted=$false
            Invoke-TaskNative $taskXperf @('-i',(Join-Path $taskDir 'cpu.etl'),'-o',(Join-Path $taskDir 'trace-stats.txt'),'-a','tracestats','-timespan')
            Invoke-TaskNative $taskXperf @('-i',(Join-Path $taskDir 'cpu.etl'),'-a','marks','-summary','-ao',(Join-Path $taskDir 'recorder-markers.txt'))
            $taskStats=Get-Content -LiteralPath (Join-Path $taskDir 'trace-stats.txt') -Raw
            $taskMarks=Get-Content -LiteralPath (Join-Path $taskDir 'recorder-markers.txt') -Raw
            $taskMatches=[regex]::Matches($taskMarks,'(?m)^\s*([\d.]+).*'+$taskMode+'-(begin|end)')
            $taskValid=($taskStats -match 'Total # Lost Events\s+: 0' -and $taskStats -match 'Total # Lost Buffers\s+: 0' -and $taskMarks -match "$taskMode-begin" -and $taskMarks -match "$taskMode-end")
            @{valid=$taskValid;mode=$taskMode;directory=$taskSpec.directory;marker_order_checked_by_dispatcher=$true} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $taskJob.FullName 'done.json') -Encoding utf8
            if ($taskMode -ne 'pilot') { [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-saved.txt'),(Get-Date -Format o)) }
            if (!$taskValid) { throw 'ETW quality check failed' }
        } catch {
            if ($taskStarted) {
                $ErrorActionPreference='Continue'
                & $taskWpr -stop (Join-Path $taskDir 'cpu-incomplete.etl') 2>&1 | Out-File -LiteralPath $taskLog -Append -Encoding utf8
                $ErrorActionPreference='Stop'
            }
            [IO.File]::WriteAllText((Join-Path $taskDir 'wpr-error.txt'),$_.ToString())
            throw
        }
    }
    Start-Sleep -Milliseconds 200
}
[IO.File]::WriteAllText((Join-Path $taskRoot 'recorder-complete.txt'),(Get-Date -Format o))
} catch {
    $_ | Out-File -LiteralPath $taskLog -Append -Encoding utf8
    [IO.File]::WriteAllText((Join-Path $taskRoot 'recorder-error.txt'),$_.ToString())
    exit 1
}
