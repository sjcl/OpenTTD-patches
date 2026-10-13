param([string]$Build = 'build-rounds', [int]$Parallel = 8)
$ErrorActionPreference = 'Stop'
$taskSource = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$taskBuild = [IO.Path]::GetFullPath((Join-Path $taskSource $Build))
$taskLock = Join-Path ([IO.Path]::GetTempPath()) 'openttd-passenger-routing-measurement.lock'
# CreateNew must succeed before any build, test, or cleanup is entered.
$taskHandle = [IO.File]::Open($taskLock, [IO.FileMode]::CreateNew, [IO.FileAccess]::ReadWrite, [IO.FileShare]::Read)
$taskOldReuse = $env:MSBUILDDISABLENODEREUSE
try {
    $taskOwner = @{ pid = $PID; output = $taskBuild; kind = 'round-verification' } | ConvertTo-Json -Compress
    $taskBytes = [Text.Encoding]::UTF8.GetBytes($taskOwner)
    $taskHandle.Write($taskBytes, 0, $taskBytes.Length); $taskHandle.Flush()
    $taskProcesses = @(Get-CimInstance Win32_Process -ErrorAction Stop)
    if ($taskProcesses.Count -lt 10) { throw 'Host process census unavailable; run outside the restricted sandbox' }
    $taskBusy = @($taskProcesses | Where-Object { $_.Name -match '^(openttd.*|cmake|MSBuild|ninja|cl|ctest|passenger_.*)\.exe$' -and $_.CommandLine -notmatch '/nodemode:' })
    if ($taskBusy.Count) { throw ('Competing CPU work: ' + ($taskBusy.ProcessId -join ', ')) }
    $env:MSBUILDDISABLENODEREUSE = '1'
    & cmake --build $taskBuild --config Release --target openttd openttd_test --parallel $Parallel *> (Join-Path $taskBuild 'verified-build.log')
    if ($LASTEXITCODE -ne 0) { throw 'Build failed; see verified-build.log' }
    & (Join-Path $taskBuild 'Release/openttd_test.exe') '[passenger]~[.benchmark]' *> (Join-Path $taskBuild 'verified-passenger.log')
    if ($LASTEXITCODE -ne 0) { throw 'Passenger regression failed; see verified-passenger.log' }
    & ctest --test-dir $taskBuild -C Release -R 'passenger_(timed_rounds|journey_active|od_load).*' --output-on-failure *> (Join-Path $taskBuild 'verified-ctest.log')
    if ($LASTEXITCODE -ne 0) { throw 'Production regression failed; see verified-ctest.log' }
    Get-Content (Join-Path $taskBuild 'verified-passenger.log') -Tail 3
    Get-Content (Join-Path $taskBuild 'verified-ctest.log') -Tail 5
} finally {
    $env:MSBUILDDISABLENODEREUSE = $taskOldReuse
    $taskHandle.Dispose()
    if (Test-Path -LiteralPath $taskLock) {
        $taskCurrentOwner = Get-Content -LiteralPath $taskLock -Raw | ConvertFrom-Json
        if ($taskCurrentOwner.pid -eq $PID) { Remove-Item -LiteralPath $taskLock -Force }
    }
}
