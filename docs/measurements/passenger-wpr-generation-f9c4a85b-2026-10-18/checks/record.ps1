$ErrorActionPreference='Stop'
& (Join-Path (Split-Path $PSScriptRoot -Parent) 'utils/passenger_wpr_record.ps1') -Build $PSScriptRoot
exit $LASTEXITCODE
