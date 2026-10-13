param(
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][string]$Label,
    [string]$InputSave = 'after-result.sav'
)
$ErrorActionPreference = 'Stop'
$taskRun = (Resolve-Path -LiteralPath $RunDirectory).Path
$taskLog = Join-Path $taskRun "$Label-console.txt"
if (Test-Path -LiteralPath $taskLog) { Remove-Item -LiteralPath $taskLog }
# Resume the same saved JPSD, queues and settings. Do not force a DB reset.
& (Join-Path $PSScriptRoot '../passenger-journey-periodic-2026-10-12/Measure-Periodic.ps1') -RunDirectory $taskRun -Binary $Binary -Label $Label -Backend periodic -InputSave $InputSave -Resume
