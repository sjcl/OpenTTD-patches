$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot
$taskFiles = @('src/console_cmds.cpp', 'src/passenger_journey_active.cpp', 'src/passenger_journey_shadow.cpp',
    'src/passenger_routing.cpp', 'src/passenger_routing.h', 'src/passenger_service_graph.cpp', 'src/passenger_service_graph.h')
$taskEditbin = 'C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/editbin.exe'
function Add-MeasurementContinuation {
    $taskSource = Join-Path $taskRoot 'src/console_cmds.cpp'
    $taskText = [IO.File]::ReadAllText($taskSource)
    $taskFunction = $taskText.IndexOf('static bool ConUnpauseGame(')
    $taskInsert = $taskText.IndexOf("`tif (_pause_mode.Test(PauseMode::Normal))", $taskFunction)
    if ($taskFunction -lt 0 -or $taskInsert -lt 0) { throw 'Unpause harness anchor missing' }
    $taskHarness = "`t/* Measurement only: accept the NewGRF continuation on this isolated copy. */`n`tif (_pause_mode.Test(PauseMode::Error)) Command<Commands::Pause>::Post(PauseMode::Error, false);`n"
    [IO.File]::WriteAllText($taskSource, $taskText.Insert($taskInsert, $taskHarness), [Text.UTF8Encoding]::new($false))
}
function Build-Measurement($taskLabel) {
    & cmake --build (Join-Path $taskRoot 'build-timetable') --config Release --target openttd --parallel 8 *> (Join-Path $PSScriptRoot "build-$taskLabel-measure.log")
    if ($LASTEXITCODE -ne 0) { throw "Build $taskLabel failed" }
    $taskBinary = Join-Path $PSScriptRoot "run/openttd-$taskLabel-measure.exe"
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Release/openttd.exe') -Destination $taskBinary
    & $taskEditbin /nologo /subsystem:console $taskBinary
    if ($LASTEXITCODE -ne 0) { throw 'Console conversion failed' }
    Get-FileHash -LiteralPath $taskBinary
}
try {
    foreach ($taskFile in $taskFiles) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "source-before/$taskFile") -Destination (Join-Path $taskRoot $taskFile); (Get-Item -LiteralPath (Join-Path $taskRoot $taskFile)).LastWriteTime = Get-Date }
    Add-MeasurementContinuation
    Build-Measurement 'before'
    foreach ($taskFile in $taskFiles) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "source-final/$taskFile") -Destination (Join-Path $taskRoot $taskFile); (Get-Item -LiteralPath (Join-Path $taskRoot $taskFile)).LastWriteTime = Get-Date }
    Add-MeasurementContinuation
    Build-Measurement 'after'
} finally {
    foreach ($taskFile in $taskFiles) { Copy-Item -LiteralPath (Join-Path $PSScriptRoot "source-final/$taskFile") -Destination (Join-Path $taskRoot $taskFile); (Get-Item -LiteralPath (Join-Path $taskRoot $taskFile)).LastWriteTime = Get-Date }
}
