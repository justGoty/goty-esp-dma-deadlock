[CmdletBinding()]
param(
    [string]$RuntimePath = (Join-Path $PSScriptRoot 'x64\Release'),
    [ValidateRange(1, 86400)][int]$DurationSeconds = 600,
    [ValidateRange(1, 60)][int]$SampleIntervalSeconds = 1,
    [ValidateRange(1, 3600)][int]$ConnectingDelaySeconds = 30,
    [int]$ClientProcessId = 0,
    [string]$OutputRoot = (Join-Path $PSScriptRoot 'diagnostics')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$RuntimePath = [IO.Path]::GetFullPath($RuntimePath)
$expectedExe = Join-Path $RuntimePath 'GOTY-Deadlock-DMA.exe'
if (-not (Test-Path -LiteralPath $RuntimePath -PathType Container)) { throw "Runtime directory missing: $RuntimePath" }
$run = Join-Path $OutputRoot ('monitor-{0}-{1}' -f (Get-Date -Format yyyyMMdd-HHmmss), $PID)
New-Item -ItemType Directory -Path $run -Force | Out-Null
$samples = Join-Path $run 'samples.csv'
$events = Join-Path $run 'events.jsonl'
$utf8 = New-Object Text.UTF8Encoding($false)
function Add-Event([string]$Kind, [string]$Detail) {
    $record = @{ utc = [DateTime]::UtcNow.ToString('o'); event = $Kind; detail = $Detail } | ConvertTo-Json -Compress
    [IO.File]::AppendAllText($events, $record + [Environment]::NewLine, $utf8)
    Write-Host "[$Kind] $Detail"
}
$watch = [Diagnostics.Stopwatch]::StartNew()
$lastPid = 0; $lastCpu = 0.0; $lastElapsed = 0.0; $previousHealth = ''; $count = 0
Add-Event 'monitor_start' $expectedExe
while ($watch.Elapsed.TotalSeconds -lt $DurationSeconds) {
    $elapsed = $watch.Elapsed.TotalSeconds
    $clients = @(Get-Process -Name 'GOTY-Deadlock-DMA' -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -eq $expectedExe -and ($ClientProcessId -eq 0 -or $_.Id -eq $ClientProcessId) } catch { $false }
    })
    if ($clients.Count -gt 1) { throw 'Multiple clients in this runtime; pass -ClientProcessId.' }
    $client = $null; $status = $null; $cpu = $null; $health = 'not_running'; $age = $null
    if ($clients.Count -eq 1) {
        $client = $clients[0]
        if ($lastPid -ne $client.Id) { Add-Event 'client_seen' "PID $($client.Id)" }
        $cpuMs = $client.TotalProcessorTime.TotalMilliseconds
        if ($lastPid -eq $client.Id -and $elapsed -gt $lastElapsed) {
            $cpu = [Math]::Round(100 * ($cpuMs - $lastCpu) / (1000 * ($elapsed - $lastElapsed) * [Environment]::ProcessorCount), 2)
        }
        $lastPid = $client.Id; $lastCpu = $cpuMs; $lastElapsed = $elapsed
        $statusFiles = @(Get-ChildItem -LiteralPath (Join-Path $RuntimePath 'logs') -Filter '*.status.json' -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending)
        foreach ($file in $statusFiles) {
            try {
                $candidate = Get-Content -LiteralPath $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
                # Guard PID reuse: status must belong to this process lifetime.
                $stamp = if ($candidate.utc -is [DateTime]) { $candidate.utc.ToUniversalTime() }
                    else { [DateTimeOffset]::Parse([string]$candidate.utc).UtcDateTime }
                if ($candidate.local_pid -eq $client.Id -and $stamp -ge $client.StartTime.ToUniversalTime()) {
                    $status = $candidate; $age = ([DateTime]::UtcNow - $stamp).TotalSeconds; break
                }
            } catch { Add-Event 'status_read_error' $_.Exception.Message }
        }
        $health = 'awaiting_heartbeat'
        if ($status) {
            $health = $status.stage
            if ($age -gt 5) { $health = 'heartbeat_stale' }
            elseif ($status.stage -eq 'connecting_dma' -and $status.PSObject.Properties['stage_age_s'] -and $status.stage_age_s -gt $ConnectingDelaySeconds) { $health = 'connecting_delay' }
            elseif ($status.stage -eq 'running' -and $status.worker_age_s -gt 5) { $health = 'worker_stale' }
            elseif ($status.stage -eq 'running' -and $status.frame_age_s -gt 5) { $health = 'ui_stale' }
        }
    } else { $lastPid = 0 }
    if ($health -ne $previousHealth) { Add-Event 'health_changed' $health; $previousHealth = $health }
    $row = [pscustomobject][ordered]@{
        Utc = [DateTime]::UtcNow.ToString('o'); ElapsedSeconds = [Math]::Round($elapsed, 2)
        LocalPid = $(if ($client) { $client.Id } else { 0 }); Health = $health; CpuPercent = $cpu
        PrivateMB = $(if ($client) { [Math]::Round($client.PrivateMemorySize64 / 1MB, 2) } else { $null })
        Handles = $(if ($client) { $client.HandleCount } else { $null })
        HeartbeatAgeSeconds = $age
        TargetPid = $(if ($status) { $status.target_pid } else { $null })
        WorkerAgeSeconds = $(if ($status) { $status.worker_age_s } else { $null })
        StageAgeSeconds = $(if ($status -and $status.PSObject.Properties['stage_age_s']) { $status.stage_age_s } else { $null })
        FailedReads = $(if ($status) { $status.failed_reads } else { $null })
        OpenScatter = $(if ($status) { $status.open_scatter } else { $null })
        SlowLoops = $(if ($status) { $status.slow_loops } else { $null })
    }
    $row | Export-Csv -LiteralPath $samples -NoTypeInformation -Append -Encoding UTF8
    ++$count
    Start-Sleep -Milliseconds ([Math]::Min($SampleIntervalSeconds * 1000, [Math]::Max(1, ($DurationSeconds - $watch.Elapsed.TotalSeconds) * 1000)))
}
Add-Event 'monitor_end' "Samples: $count; directory: $run"
Write-Output $run
