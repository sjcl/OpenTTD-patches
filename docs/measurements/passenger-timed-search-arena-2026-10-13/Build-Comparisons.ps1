param([Parameter(Mandatory=$true)][string]$Source, [Parameter(Mandatory=$true)][string]$Build,
      [Parameter(Mandatory=$true)][string]$Dependencies, [int]$Parallel = 8,
      [string[]]$Modes = @('legacy','arena','disabled','legacy-detail','arena-detail','network'), [switch]$PreserveProduct)
$ErrorActionPreference = 'Stop'
$taskSource = (Resolve-Path -LiteralPath $Source).Path
$taskBuild = (Resolve-Path -LiteralPath $Build).Path
if (!$PreserveProduct) { Copy-Item -LiteralPath (Join-Path $taskBuild 'Release/openttd.exe') -Destination (Join-Path $taskBuild 'openttd-product.exe') }
$env:MSBUILDDISABLENODEREUSE = '1'
foreach ($taskMode in $Modes) {
    & cmake -S $taskSource -B $taskBuild -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$Dependencies" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPTION_NO_LTO=ON -DOPTION_USE_ASSERTS=ON -DOPTION_PASSENGER_BENCHMARK=ON *> (Join-Path $taskBuild "configure-$taskMode.log")
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $taskMode" }
    & python (Join-Path $PSScriptRoot 'Prepare-Comparison.py') $taskSource $taskBuild --mode $taskMode
    if ($LASTEXITCODE -ne 0) { throw "Instrumentation failed: $taskMode" }
    if ($taskMode -eq 'network') {
        & python (Join-Path $taskSource 'docs/measurements/passenger-timed-pruning-2026-10-13/Prepare-Network-Probe.py') $taskSource $taskBuild
        if ($LASTEXITCODE -ne 0) { throw 'Network probe preparation failed' }
    }
    & cmake --build $taskBuild --config Release --target openttd --parallel $Parallel *> (Join-Path $taskBuild "build-$taskMode.log")
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $taskMode" }
    $taskBinary = Join-Path $taskBuild "openttd-$taskMode.exe"
    Copy-Item -LiteralPath (Join-Path $taskBuild 'Release/openttd.exe') -Destination $taskBinary
    Copy-Item -LiteralPath (Join-Path $taskBuild 'journey-profile/instrumentation.json') -Destination (Join-Path $taskBuild "openttd-$taskMode.instrumentation.json")
    $taskBytes = [IO.File]::ReadAllBytes($taskBinary)
    $taskPE = [BitConverter]::ToInt32($taskBytes, 0x3c)
    if ([BitConverter]::ToUInt16($taskBytes, $taskPE + 24 + 68) -ne 2) { throw 'Expected GUI subsystem' }
    $taskBytes[$taskPE + 24 + 68] = 3
    [IO.File]::WriteAllBytes($taskBinary, $taskBytes)
    Write-Output "$taskMode $((Get-FileHash -LiteralPath $taskBinary).Hash)"
}
