$ErrorActionPreference='Stop'
$taskWork=Join-Path $PSScriptRoot 'rebuild'
$taskRecorder=Join-Path (Split-Path $PSScriptRoot -Parent) 'utils/passenger_wpr_record.ps1'
foreach ($taskMode in @('legacy','hyperpath')) {
    foreach ($taskVersion in @('baseline','candidate')) {
        & $taskRecorder -Build (Join-Path $taskWork $taskVersion) -Mode $taskMode
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    }
}
[IO.File]::WriteAllText((Join-Path $taskWork 'matrix-recorder-complete.txt'),(Get-Date -Format o))
