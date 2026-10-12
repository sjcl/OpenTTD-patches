$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot
$taskRun = Join-Path $PSScriptRoot 'run'
$taskOutput = Join-Path $taskRoot 'docs/measurements/passenger-journey-timetable-assumption-2026-10-12'
New-Item -ItemType Directory -Force -Path $taskOutput | Out-Null
function Read-Metrics($taskLabel) {
    $taskConsole = [IO.File]::ReadAllText((Join-Path $taskRun "$taskLabel-console.txt"))
    function Read-Values($taskPattern, $taskKeys) {
        $taskMatch = [regex]::Match($taskConsole, $taskPattern)
        if (!$taskMatch.Success) { throw "Missing metric: $taskPattern" }
        $taskValues = [ordered]@{}
        for ($taskIndex = 0; $taskIndex -lt $taskKeys.Count; ++$taskIndex) { $taskValues[$taskKeys[$taskIndex]] = [long]$taskMatch.Groups[$taskIndex + 1].Value }
        return $taskValues
    }
    $taskProcess = Get-Content -LiteralPath (Join-Path $taskRun "$taskLabel-process.json") -Raw | ConvertFrom-Json
    $taskInventory = [regex]::Matches($taskConsole, 'Passenger inventory: complete; waiting=(\d+) people .*?deferred=(\d+) people; state_ticks=(\d+); scaled_ticks=(\d+)')
    if ($taskInventory.Count -ne 2) { throw 'Expected exactly two complete inventories' }
    $taskInventoryValues = @($taskInventory | ForEach-Object { [ordered]@{waiting=[long]$_.Groups[1].Value; deferred=[long]$_.Groups[2].Value; state_ticks=[long]$_.Groups[3].Value; scaled_ticks=[long]$_.Groups[4].Value} })
    if ($taskProcess.exit_code -ne 0 -or $taskInventoryValues[0].state_ticks -ne 3176545487 -or $taskInventoryValues[1].state_ticks -ne 3176547014) { throw 'Incomplete measurement interval' }
    $taskFallbacks = [ordered]@{}
    foreach ($taskMatch in [regex]::Matches($taskConsole, 'Fallback (\w+): (\d+) packets; deferred=(\d+) packets')) {
        $taskFallbacks[$taskMatch.Groups[1].Value] = [ordered]@{packets=[long]$taskMatch.Groups[2].Value; deferred=[long]$taskMatch.Groups[3].Value}
    }
    return [ordered]@{
        process=$taskProcess
        inventory=$taskInventoryValues
        waiting_routing=Read-Values 'Waiting routing: timed=(\d+); CargoDist_or_deferred=(\d+); OD_fallback_or_waiting=(\d+) people\s*(?:Console log file closed\.|Graph coverage:)' @('timed','CargoDist_or_deferred','OD_fallback_or_waiting')
        active=Read-Values 'Active journey total: applied=(\d+) packets/(\d+) passengers; builds=(\d+); static_hits=(\d+); searches=(\d+); OD_hits=(\d+)' @('applied_packets','applied_passengers','builds','static_hits','searches','OD_hits')
        work_time=Read-Values 'Graph work=(\d+); mean=(\d+); max=(\d+); cached_index_allowance_avoided=(\d+); query_allowance=(\d+); build_us=(\d+); mean_us=(\d+); max_us=(\d+); search_us=(\d+); max_tick_graph_search_us=(\d+)' @('graph_work','mean_graph_work','max_graph_work','cached_index_work_avoided','query_allowance','build_us','mean_build_us','max_build_us','search_us','max_tick_graph_search_us')
        simulation=Read-Values 'Simulation tick time: completed_ticks=(\d+); total_us=(\d+); mean_us=(\d+); max_us=(\d+)' @('completed_ticks','total_us','mean_us','max_us')
        graph_memory=Read-Values 'Peak graph retained_estimate=(\d+); static retained_estimate=(\d+); conservative_graph_envelope_including_index=(\d+); query_scratch=(\d+); query_cache=(\d+)' @('retained','static_retained','envelope','query_scratch','query_cache')
        timed_transport=Read-Values 'Journey transport: boarded=(\d+); transferred=(\d+); delivered=(\d+) people' @('boarded','transferred','delivered')
        untimed_transport=Read-Values 'Untimed transport: boarded=(\d+); transferred=(\d+); delivered=(\d+) people' @('boarded','transferred','delivered')
        routing=Read-Values 'Pragmatic routing: timed=(\d+) packets; timed_ignoring_unknown=(\d+); timed_budget_deferred=(\d+); untimed=(\d+) live routes; untimed_no_route=(\d+); untimed_budget_deferred=(\d+)' @('timed_packets','timed_ignoring_unknown','timed_budget_deferred','untimed_routes','untimed_no_route','untimed_budget_deferred')
        uncertainty=Read-Values 'Uncertainty: timing=(\d+) trips; path=(\d+) trips' @('timing','path')
        timing_stops=Read-Values 'Trip stop Timing: (\d+) predictions' @('predictions')
        od_fallback=Read-Values 'OD fallback: work=(\d+); path_searches=(\d+)' @('work','path_searches')
        fallbacks=$taskFallbacks
        inspect=@([regex]::Matches($taskConsole, 'Prediction [^\r\n]+') | ForEach-Object { $_.Value })
        save_sha256=(Get-FileHash -LiteralPath (Join-Path $taskRun "$taskLabel-result.sav")).Hash.ToLowerInvariant()
        binary_sha256=(Get-FileHash -LiteralPath (Join-Path $taskRun "openttd-$taskLabel-measure.exe")).Hash.ToLowerInvariant()
    }
}
$taskSourceHashes = [ordered]@{}
foreach ($taskFile in @('src/console_cmds.cpp','src/passenger_journey_active.cpp','src/passenger_journey_shadow.cpp','src/passenger_routing.cpp','src/passenger_routing.h','src/passenger_service_graph.cpp','src/passenger_service_graph.h')) {
    $taskCurrentHash = (Get-FileHash -LiteralPath (Join-Path $taskRoot $taskFile)).Hash
    if ($taskCurrentHash -ne (Get-FileHash -LiteralPath (Join-Path $PSScriptRoot "source-final/$taskFile")).Hash) { throw "Unrestored source: $taskFile" }
    $taskSourceHashes[$taskFile] = $taskCurrentHash.ToLowerInvariant()
}
$taskMetadata = [ordered]@{
    date='2026-10-12'; timezone='Asia/Tokyo'; branch='feat/passenger-journey-timetable-assumption'
    base_commit='1556b330cd9360d1e2f6b0ad50e45b70b86c6341'
    input_sha256=(Get-FileHash -LiteralPath (Join-Path $taskRun 'input.sav')).Hash.ToLowerInvariant()
    conditions=[ordered]@{build='MSVC 19.44 x64 Release; assertions ON; LTO OFF; static dependencies'; drivers='null:until_exit / null / null'; sequential_runs=$true; harness='Console unpause clears existing NewGRF error pause on an isolated input copy only'; distribution_pax=2; passenger_od_assignment=$true; passenger_journey_routing=$true; recalc_interval=8; recalc_time=32}
    validation=[ordered]@{passenger_cases=209; passenger_assertions=937813; ctest_passed=424; initial_ctest_passed=421; asset_repair_retest_passed=3; restored_product_save_load_passed=3; live_multiplayer_soak=$false; gui_visual_render=$false}
    before=Read-Metrics 'before'; after=Read-Metrics 'after'
    original_input_unchanged=$true
    source_restored=$true
    source_sha256=$taskSourceHashes
    config_sha256=(Get-FileHash -LiteralPath (Join-Path $taskRun 'openttd.cfg')).Hash.ToLowerInvariant()
    final_product_binary_sha256=(Get-FileHash -LiteralPath (Join-Path $PSScriptRoot 'Release/openttd.exe')).Hash.ToLowerInvariant()
}
if ($taskMetadata.input_sha256 -ne '14d68c82911b9dcfc5ef7291e9697c5bbd67f44ffcc79d7575a144405edf6390') { throw 'Input hash mismatch' }
$taskMetadata | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $taskOutput 'metadata.json') -Encoding utf8
foreach ($taskLabel in @('before','after')) {
    foreach ($taskSuffix in @('console.txt','process.json','stderr.txt')) { Copy-Item -LiteralPath (Join-Path $taskRun "$taskLabel-$taskSuffix") -Destination $taskOutput }
    foreach ($taskSuffix in @('start.scr','finish.scr')) { Copy-Item -LiteralPath (Join-Path $taskRun "scripts/$taskLabel-$taskSuffix") -Destination $taskOutput }
}
foreach ($taskFile in @('passenger-tests.log','ctest.log','ctest-retry.log','restored-product-tests.log','Build-Measurement.ps1','Create-Metadata.ps1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $taskFile) -Destination $taskOutput
}
Copy-Item -LiteralPath (Join-Path $taskRun 'Measure-Journey.ps1') -Destination $taskOutput
$taskMetadata | ConvertTo-Json -Depth 3
