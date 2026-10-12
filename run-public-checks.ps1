[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$checks = @(
    'run-self-checks.ps1',
    'run-global-pattern-checks.ps1',
    'run-render-checks.ps1',
    'run-short-wait-checks.ps1',
    'run-stop-signal-checks.ps1',
    'run-display-awake-checks.ps1',
    'run-target-liveness-checks.ps1',
    'run-managed-refresh-checks.ps1',
    'run-refresh-experiment-checks.ps1',
    'tests/check-monitor.ps1'
)
foreach ($check in $checks) {
    Write-Host "Checking $check"
    & (Join-Path $PSScriptRoot $check)
    if ($LASTEXITCODE -ne 0) { throw "Check failed: $check" }
}
Write-Host 'PASS: all public snapshot checks completed without DMA hardware.'
