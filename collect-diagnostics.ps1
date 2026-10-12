[CmdletBinding()]
param(
    [string]$RuntimePath = (Join-Path $PSScriptRoot 'x64\Release'),
    [string]$OutputRoot = (Join-Path $PSScriptRoot 'diagnostics')
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$RuntimePath = [IO.Path]::GetFullPath($RuntimePath)
if (-not (Test-Path -LiteralPath $RuntimePath -PathType Container)) { throw "Runtime directory missing: $RuntimePath" }
$run = Join-Path $OutputRoot ('GOTY-Deadlock-{0}-{1}' -f (Get-Date -Format yyyyMMdd-HHmmss), $PID)
New-Item -ItemType Directory -Path $run -Force | Out-Null
function Get-PeMachine([string]$FilePath) {
    $reader = $null
    try {
        $reader = New-Object IO.BinaryReader([IO.File]::Open($FilePath, 'Open', 'Read', 'ReadWrite'))
        if ($reader.BaseStream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { return 'not_pe' }
        $reader.BaseStream.Position = 0x3c; $offset = $reader.ReadInt32()
        if ($offset -lt 0 -or $offset + 6 -gt $reader.BaseStream.Length) { return 'invalid_pe' }
        $reader.BaseStream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550) { return 'invalid_pe' }
        switch ($reader.ReadUInt16()) { 0x8664 { return 'x64' }; 0x14c { return 'x86' }; default { return 'other' } }
    } catch { return 'unreadable' } finally { if ($reader) { $reader.Dispose() } }
}
function Copy-NativeLogTail([string]$FilePath, [string]$Destination) {
    $source = $null; $target = $null
    try {
        $source = [IO.File]::Open($FilePath, [IO.FileMode]::Open, [IO.FileAccess]::Read, ([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
        $length = $source.Length
        $limit = 8MB
        $start = [Math]::Max(0L, $length - $limit)
        $source.Position = $start
        $target = [IO.File]::Open($Destination, [IO.FileMode]::Create, [IO.FileAccess]::Write, [IO.FileShare]::Read)
        $remaining = $length - $start; $copied = 0L
        $buffer = New-Object byte[] 65536
        while ($remaining -gt 0) {
            $count = $source.Read($buffer, 0, [int][Math]::Min($buffer.Length, $remaining))
            if ($count -eq 0) { break }
            $target.Write($buffer, 0, $count)
            $copied += $count; $remaining -= $count
        }
        return [ordered]@{ File = [IO.Path]::GetFileName($FilePath); SourceBytes = $length; CopiedBytes = $copied; Truncated = ($start -gt 0); Incomplete = ($remaining -gt 0) }
    } finally {
        if ($target) { $target.Dispose() }
        if ($source) { $source.Dispose() }
    }
}
$names = 'GOTY-Deadlock-DMA.exe', 'vmm.dll', 'leechcore.dll', 'leechcore_driver.dll', 'FTD3XX.dll', 'FTD3XXWU.dll'
$manifest = foreach ($name in $names) {
    $path = Join-Path $RuntimePath $name; $present = Test-Path -LiteralPath $path -PathType Leaf
    [pscustomobject]@{
        Name = $name; Present = $present
        Architecture = $(if ($present) { Get-PeMachine $path } else { '' })
        Sha256 = $(if ($present) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash } else { '' })
        FileVersion = $(if ($present) { [Diagnostics.FileVersionInfo]::GetVersionInfo($path).FileVersion } else { '' })
    }
}
$manifest | Export-Csv -LiteralPath (Join-Path $run 'manifest.csv') -NoTypeInformation -Encoding UTF8
$statusFiles = @(Get-ChildItem -LiteralPath (Join-Path $RuntimePath 'logs') -Filter '*.status.json' -ErrorAction SilentlyContinue | Sort-Object LastWriteTimeUtc -Descending)
$nativeLogCapture = $null
if ($statusFiles.Count) {
    $latest = $statusFiles[0]
    Copy-Item -LiteralPath $latest.FullName -Destination $run
    $session = $latest.Name.Substring(0, $latest.Name.Length - '.status.json'.Length)
    $sessionLogs = @(Get-ChildItem -LiteralPath (Join-Path $RuntimePath 'logs') -Filter "$session.*.jsonl")
    foreach ($log in $sessionLogs) { Copy-Item -LiteralPath $log.FullName -Destination $run }
    $crash = Join-Path $RuntimePath "logs\$session.crash.txt"
    if ((Test-Path -LiteralPath $crash) -and (Get-Item -LiteralPath $crash).Length -gt 0) {
        Copy-Item -LiteralPath $crash -Destination $run
    }
    $nativeLog = Join-Path $RuntimePath "logs\$session.memprocfs.log"
    if (Test-Path -LiteralPath $nativeLog -PathType Leaf) {
        try { $nativeLogCapture = Copy-NativeLogTail $nativeLog (Join-Path $run "$session.memprocfs.log") }
        catch { $nativeLogCapture = [ordered]@{ File = "$session.memprocfs.log"; Error = $_.Exception.Message } }
    }
}
$report = [ordered]@{
    CollectedUtc = [DateTime]::UtcNow.ToString('o'); RuntimePath = $RuntimePath
    OS = [Environment]::OSVersion.VersionString; LogicalProcessors = [Environment]::ProcessorCount
    MissingFiles = @($manifest | Where-Object { -not $_.Present } | ForEach-Object { $_.Name })
    WrongArchitecture = @($manifest | Where-Object { $_.Present -and $_.Architecture -ne 'x64' } | ForEach-Object { $_.Name })
    MapPresent = (Test-Path -LiteralPath (Join-Path $RuntimePath 'Resources\Map.png'))
    NativeLogCapture = $nativeLogCapture
    Note = 'No config, target memory, credentials or crash dump collected. DLL hashes identify the exact runtime set.'
    NativeLogNote = 'Only the current session native log is collected, bounded to its last 8 MiB; the first tail line may be partial.'
}
try {
    $report.Ft601Devices = @(Get-PnpDevice -PresentOnly -ErrorAction Stop | Where-Object { $_.FriendlyName -match 'FT601|FTDI|FT60' } | Select-Object FriendlyName, Status, Class)
} catch { $report.DeviceQueryError = $_.Exception.Message }
$report | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $run 'report.json') -Encoding UTF8
$zip = "$run.zip"
Compress-Archive -Path (Join-Path $run '*') -DestinationPath $zip -CompressionLevel Optimal
Write-Output $zip
