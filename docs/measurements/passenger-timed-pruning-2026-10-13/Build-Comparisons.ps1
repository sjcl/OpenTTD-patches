param([Parameter(Mandatory=$true)][string]$Source, [Parameter(Mandatory=$true)][string]$Build,
      [Parameter(Mandatory=$true)][string]$Dependencies, [int]$Parallel = 8)
$ErrorActionPreference = 'Stop'
$taskSource = (Resolve-Path -LiteralPath $Source).Path
$taskBuild = (Resolve-Path -LiteralPath $Build).Path
Copy-Item -LiteralPath (Join-Path $taskBuild 'Release/openttd.exe') -Destination (Join-Path $taskBuild 'openttd-product.exe')
$env:MSBUILDDISABLENODEREUSE = '1'
foreach ($taskMode in @('baseline','pruned','baseline-detail','detail')) {
    & cmake -S $taskSource -B $taskBuild -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$Dependencies" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPTION_NO_LTO=ON -DOPTION_USE_ASSERTS=ON -DOPTION_PASSENGER_BENCHMARK=ON *> (Join-Path $taskBuild "configure-$taskMode.log")
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $taskMode" }
    & python (Join-Path $PSScriptRoot 'Prepare-Comparison.py') $taskSource $taskBuild --mode $taskMode
    if ($LASTEXITCODE -ne 0) { throw "Instrumentation failed: $taskMode" }
    & cmake --build $taskBuild --config Release --target openttd --parallel $Parallel *> (Join-Path $taskBuild "build-$taskMode.log")
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $taskMode" }
    $taskBinary = Join-Path $taskBuild "openttd-$taskMode.exe"
    Copy-Item -LiteralPath (Join-Path $taskBuild 'Release/openttd.exe') -Destination $taskBinary
    Copy-Item -LiteralPath (Join-Path $taskBuild 'journey-profile/instrumentation.json') -Destination (Join-Path $taskBuild "openttd-$taskMode.instrumentation.json")
    # GUI -> console only; code and simulation behavior are unchanged.
    $taskBytes = [IO.File]::ReadAllBytes($taskBinary)
    $taskPE = [BitConverter]::ToInt32($taskBytes, 0x3c)
    if ([BitConverter]::ToUInt16($taskBytes, $taskPE + 24 + 68) -ne 2) { throw 'Expected Windows GUI subsystem' }
    $taskBytes[$taskPE + 24 + 68] = 3
    [IO.File]::WriteAllBytes($taskBinary, $taskBytes)
    Write-Output "$taskMode $((Get-FileHash -LiteralPath $taskBinary).Hash)"
}
