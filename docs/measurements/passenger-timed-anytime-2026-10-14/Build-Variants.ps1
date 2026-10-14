param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Build,
    [Parameter(Mandatory=$true)][string]$Dependencies,
    [string[]]$Modes = @('anytime','legacy','quality','network','continuation','original'),
    [int]$Parallel = 8
)
$ErrorActionPreference = 'Stop'
$taskSource = (Resolve-Path -LiteralPath $Source).Path
$taskBuild = [IO.Path]::GetFullPath($Build)
$taskPreviousNodeReuse = $env:MSBUILDDISABLENODEREUSE
try {
    $env:MSBUILDDISABLENODEREUSE = '1'
    foreach ($taskMode in $Modes) {
        if ($taskMode -notin @('anytime','legacy','quality','network','continuation','original')) { throw "Unknown mode: $taskMode" }
        & cmake -S $taskSource -B $taskBuild -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$Dependencies" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPTION_NO_LTO=ON -DOPTION_USE_ASSERTS=ON -DOPTION_PASSENGER_BENCHMARK=ON *> (Join-Path $taskBuild "configure-final-$taskMode.log")
        if ($LASTEXITCODE -ne 0) { throw "Configure failed: $taskMode" }
        & python (Join-Path $taskSource 'docs/measurements/passenger-routing-profile-2026-10-13/Instrument-Profile.py') $taskSource $taskBuild --mode minimal *> (Join-Path $taskBuild "instrument-final-$taskMode.log")
        if ($LASTEXITCODE -ne 0) { throw "Instrumentation failed: $taskMode" }
        if ($taskMode -eq 'legacy') { & python (Join-Path $PSScriptRoot 'Prepare-Legacy.py') $taskSource $taskBuild }
        elseif ($taskMode -eq 'original') { & python (Join-Path $PSScriptRoot 'Prepare-Legacy.py') $taskSource $taskBuild --original }
        elseif ($taskMode -eq 'quality') { & python (Join-Path $PSScriptRoot 'Prepare-Quality.py') $taskSource $taskBuild }
        elseif ($taskMode -eq 'network') { & python (Join-Path $taskSource 'docs/measurements/passenger-timed-pruning-2026-10-13/Prepare-Network-Probe.py') $taskSource $taskBuild }
        elseif ($taskMode -eq 'continuation') { & python (Join-Path $PSScriptRoot 'Prepare-Continuation.py') $taskSource $taskBuild }
        if ($LASTEXITCODE -ne 0) { throw "Mode preparation failed: $taskMode" }
        # The original and replacement TUs use the same object names. Remove
        # only these owned build outputs to prevent stale tracking across modes.
        foreach ($taskObjectName in @('passenger_service_database.obj', 'console_cmds.obj', 'null_v.obj')) {
            $taskObjectPath = Join-Path $taskBuild "openttd_lib.dir/Release/$taskObjectName"
            if (Test-Path -LiteralPath $taskObjectPath) { Remove-Item -LiteralPath $taskObjectPath }
        }
        [string[]]$taskTargets = if ($taskMode -eq 'anytime') { @('openttd','openttd_test','regression_files') } else { @('openttd') }
        & cmake --build $taskBuild --config Release --target @taskTargets --parallel $Parallel *> (Join-Path $taskBuild "build-final-$taskMode.log")
        if ($LASTEXITCODE -ne 0) { throw "Build failed: $taskMode" }
        $taskBinary = Join-Path $taskBuild "openttd-$taskMode.exe"
        Copy-Item -LiteralPath (Join-Path $taskBuild 'Release/openttd.exe') -Destination $taskBinary
        Copy-Item -LiteralPath (Join-Path $taskBuild 'journey-profile/instrumentation.json') -Destination (Join-Path $taskBuild "openttd-$taskMode.instrumentation.json")
        if ($taskMode -eq 'network') {
            # The existing loopback harness needs a console subsystem for stdin.
            $taskBytes = [IO.File]::ReadAllBytes($taskBinary)
            $taskPE = [BitConverter]::ToInt32($taskBytes, 0x3c)
            if ([BitConverter]::ToUInt16($taskBytes, $taskPE + 24 + 68) -ne 2) { throw 'Expected GUI subsystem' }
            $taskBytes[$taskPE + 24 + 68] = 3
            [IO.File]::WriteAllBytes($taskBinary, $taskBytes)
        }
        Write-Output "$taskMode $((Get-FileHash -LiteralPath $taskBinary).Hash)"
        if ($taskMode -eq 'anytime') {
            & (Join-Path $taskBuild 'Release/openttd_test.exe') '[passenger]~[.benchmark]' --reporter compact *> (Join-Path $taskBuild 'passenger-final-tests.log')
            if ($LASTEXITCODE -ne 0) { throw 'Passenger regression failed' }
        }
    }
} finally {
    $env:MSBUILDDISABLENODEREUSE = $taskPreviousNodeReuse
}
