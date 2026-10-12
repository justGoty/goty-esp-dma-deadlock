$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$fixture = Join-Path $repo ('diagnostics\fixture-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $fixture -Force | Out-Null
$command = @"
@echo off
call "$vcvars" >nul
cd /d "$fixture"
cl /nologo /std:c++latest /EHsc /utf-8 /I"$repo\Deadlock_DMA" /I"$repo\Dependencies\nlohmann" "$PSScriptRoot\monitor_fixture.cpp" "$repo\Deadlock_DMA\Diagnostics\Diagnostics.cpp" /Fe:"$fixture\GOTY-Deadlock-DMA.exe"
exit /b %errorlevel%
"@
$cmdPath = Join-Path $fixture 'compile.cmd'
[IO.File]::WriteAllText($cmdPath, $command, [Text.Encoding]::Default)
& $env:ComSpec /d /c $cmdPath
if ($LASTEXITCODE -ne 0) { throw 'Fixture compilation failed.' }
$client = Start-Process -FilePath (Join-Path $fixture 'GOTY-Deadlock-DMA.exe') -ArgumentList 'stale' -WindowStyle Hidden -PassThru
try {
    $monitorResult = & (Join-Path $repo 'monitor-live-diagnostics.ps1') -RuntimePath $fixture -DurationSeconds 8 -ClientProcessId $client.Id
    $samples = @(Import-Csv -LiteralPath (Join-Path $monitorResult 'samples.csv'))
    if (-not @($samples | Where-Object Health -eq 'running').Count) { throw 'Monitor did not detect running client.' }
    if (-not @($samples | Where-Object Health -eq 'worker_stale').Count) { throw 'Monitor did not detect stale worker.' }
    $latest = Get-ChildItem -LiteralPath (Join-Path $fixture 'logs') -Filter '*.status.json' | Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    $session = (Get-Content -LiteralPath $latest.FullName -Raw -Encoding UTF8 | ConvertFrom-Json).session
    $nativeLog = Join-Path $fixture "logs\$session.memprocfs.log"
    $stream = [IO.File]::Open($nativeLog, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
    try {
        $block = [Text.Encoding]::UTF8.GetBytes(('N' * 1MB))
        for ($i = 0; $i -lt 9; ++$i) { $stream.Write($block, 0, $block.Length) }
        $suffix = [Text.Encoding]::UTF8.GetBytes("`nSYNTHETIC-NATIVE-LOG-END`n")
        $stream.Write($suffix, 0, $suffix.Length)
    } finally { $stream.Dispose() }
    [IO.File]::WriteAllText((Join-Path $fixture 'logs\old-session.memprocfs.log'), 'Do not collect unrelated sessions')
    $zip = & (Join-Path $repo 'collect-diagnostics.ps1') -RuntimePath $fixture
    if (-not (Test-Path -LiteralPath $zip)) { throw 'Collector did not create ZIP.' }
    $extracted = Join-Path $fixture 'collected'
    Expand-Archive -LiteralPath $zip -DestinationPath $extracted
    $nativeFiles = @(Get-ChildItem -LiteralPath $extracted -Filter '*.memprocfs.log')
    if ($nativeFiles.Count -ne 1 -or $nativeFiles[0].Name -ne "$session.memprocfs.log" -or $nativeFiles[0].Length -ne 8MB) { throw 'Collector native log session filter or 8 MiB bound failed.' }
    $tail = [IO.File]::Open($nativeFiles[0].FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try {
        $tail.Position = $tail.Length - $suffix.Length
        $actual = New-Object byte[] $suffix.Length
        if ($tail.Read($actual, 0, $actual.Length) -ne $actual.Length -or [Text.Encoding]::UTF8.GetString($actual) -ne [Text.Encoding]::UTF8.GetString($suffix)) { throw 'Collector lost native log tail.' }
    } finally { $tail.Dispose() }
    $report = Get-Content -LiteralPath (Join-Path $extracted 'report.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $report.NativeLogCapture.Truncated -or $report.NativeLogCapture.CopiedBytes -ne 8MB -or $report.NativeLogCapture.Incomplete) { throw 'Collector truncation metadata failed.' }
    Write-Host 'PASS: monitor found running client and stale worker; collector preserved current native log tail within 8 MiB.'
} finally { $client.WaitForExit(15000) | Out-Null }
$client = Start-Process -FilePath (Join-Path $fixture 'GOTY-Deadlock-DMA.exe') -ArgumentList 'connecting' -WindowStyle Hidden -PassThru
try {
    $monitorResult = & (Join-Path $repo 'monitor-live-diagnostics.ps1') -RuntimePath $fixture -DurationSeconds 4 -ClientProcessId $client.Id -ConnectingDelaySeconds 2
    $samples = @(Import-Csv -LiteralPath (Join-Path $monitorResult 'samples.csv'))
    if (-not @($samples | Where-Object Health -eq 'connecting_dma').Count -or -not @($samples | Where-Object Health -eq 'connecting_delay').Count) { throw 'Monitor did not recognize a delayed native initialization.' }
    Write-Host 'PASS: monitor detects connection delay without cancelling or misclassifying the worker.'
} finally { $client.WaitForExit(15000) | Out-Null }
$client = Start-Process -FilePath (Join-Path $fixture 'GOTY-Deadlock-DMA.exe') -ArgumentList 'legacy' -WindowStyle Hidden -PassThru
try {
    $monitorResult = & (Join-Path $repo 'monitor-live-diagnostics.ps1') -RuntimePath $fixture -DurationSeconds 3 -ClientProcessId $client.Id -ConnectingDelaySeconds 1
    $samples = @(Import-Csv -LiteralPath (Join-Path $monitorResult 'samples.csv'))
    if (-not @($samples | Where-Object Health -eq 'connecting_dma').Count -or @($samples | Where-Object Health -eq 'connecting_delay').Count) { throw 'Monitor is incompatible with legacy status files without stage_age_s.' }
    if (@(Get-Content -LiteralPath (Join-Path $monitorResult 'events.jsonl') | Where-Object { $_ -match 'status_read_error' }).Count) { throw 'Legacy status produced monitor read errors.' }
    Write-Host 'PASS: monitor remains compatible with 0.1.1 status files without stage_age_s.'
} finally { $client.WaitForExit(15000) | Out-Null }
