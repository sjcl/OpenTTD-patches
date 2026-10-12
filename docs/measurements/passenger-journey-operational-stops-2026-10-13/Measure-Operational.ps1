param(
    [Parameter(Mandatory=$true)][string]$RunDirectory,
    [Parameter(Mandatory=$true)][string]$Binary,
    [Parameter(Mandatory=$true)][ValidateSet('base','base2','after','after2')][string]$Label
)
$ErrorActionPreference = 'Stop'
$taskRun = (Resolve-Path -LiteralPath $RunDirectory).Path
# The shared harness appends console logs. Reset only this run's known output.
$taskConsole = Join-Path $taskRun "$Label-console.txt"
if (Test-Path -LiteralPath $taskConsole) { Remove-Item -LiteralPath $taskConsole }
& (Join-Path $PSScriptRoot '../passenger-journey-periodic-2026-10-12/Measure-Periodic.ps1') -RunDirectory $taskRun -Binary $Binary -Label $Label -Backend periodic
