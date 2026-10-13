param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Build,
    [Parameter(Mandatory=$true)][string]$Dependencies,
    [ValidateSet('minimal','detail')][string]$Mode = 'minimal',
    [int]$Parallel = 8
)
$ErrorActionPreference = 'Stop'
$taskSource = (Resolve-Path -LiteralPath $Source).Path
$taskBuild = [IO.Path]::GetFullPath($Build)
New-Item -ItemType Directory -Path $taskBuild -Force | Out-Null
& cmake -S $taskSource -B $taskBuild -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$Dependencies" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DOPTION_NO_LTO=ON -DOPTION_USE_ASSERTS=ON -DOPTION_PASSENGER_BENCHMARK=ON *> (Join-Path $taskBuild "configure-$Mode.log")
if ($LASTEXITCODE -ne 0) { throw "Configure failed; see configure-$Mode.log" }
& python (Join-Path $PSScriptRoot 'Instrument-Profile.py') $taskSource $taskBuild --mode $Mode
if ($LASTEXITCODE -ne 0) { throw 'Instrumentation anchor check failed' }
$taskOldNodeReuse = $env:MSBUILDDISABLENODEREUSE
try {
    $env:MSBUILDDISABLENODEREUSE = '1'
    & cmake --build $taskBuild --config Release --target openttd --parallel $Parallel *> (Join-Path $taskBuild "build-$Mode.log")
    if ($LASTEXITCODE -ne 0) { throw "Build failed; see build-$Mode.log" }
} finally {
    $env:MSBUILDDISABLENODEREUSE = $taskOldNodeReuse
}
$taskBinary = Join-Path $taskBuild "openttd-$Mode.exe"
Copy-Item -LiteralPath (Join-Path $taskBuild 'Release/openttd.exe') -Destination $taskBinary
Copy-Item -LiteralPath (Join-Path $taskBuild 'journey-profile/instrumentation.json') -Destination (Join-Path $taskBuild "openttd-$Mode.instrumentation.json")
(Get-FileHash -LiteralPath $taskBinary).Hash
