[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools not found.' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $PSScriptRoot 'diagnostics\offline'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$cmdPath = Join-Path $output 'compile.cmd'
# A fixed compiler command, with quoted workspace paths and no user-provided shell fragments.
$command = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$output"
cl /nologo /std:c++latest /EHsc /utf-8 /W4 /wd4200 /I"$PSScriptRoot\Deadlock_DMA" /I"$PSScriptRoot\Dependencies\MemProcFS" /I"$PSScriptRoot\Dependencies\nlohmann" "$PSScriptRoot\tests\offline_tests.cpp" "$PSScriptRoot\Deadlock_DMA\Diagnostics\Diagnostics.cpp" /Fe:"$output\offline-tests.exe"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($cmdPath, $command, [Text.Encoding]::Default)
& $env:ComSpec /d /c $cmdPath
if ($LASTEXITCODE -ne 0) { throw 'Offline tests compilation failed.' }
$run = Join-Path $output ('run-' + [Guid]::NewGuid().ToString('N'))
& (Join-Path $output 'offline-tests.exe') $run
if ($LASTEXITCODE -ne 0) { throw 'Offline tests failed.' }
$crashRun = Join-Path $output ('crash-' + [Guid]::NewGuid().ToString('N'))
$PSNativeCommandUseErrorActionPreference = $false
& (Join-Path $output 'offline-tests.exe') $crashRun crash
if ($LASTEXITCODE -eq 0) { throw 'Native crash fixture did not fail.' }
$crashFiles = @(Get-ChildItem -LiteralPath $crashRun -Filter '*.crash.txt')
if ($crashFiles.Count -ne 1 -or (Get-Content -LiteralPath $crashFiles[0].FullName -Raw) -notmatch 'exception=0xC0000005') {
    throw 'Native crash record missing.'
}
Write-Host 'PASS: native crash recorded without logger locks or memory dump.'
# The intentional native crash is an expected test result, not the script exit status.
$global:LASTEXITCODE = 0

$performanceCommandPath = Join-Path $output 'compile-perf.cmd'
$performanceCommand = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$output"
cl /nologo /std:c++latest /EHsc /utf-8 /W4 /I"$PSScriptRoot\Deadlock_DMA" /I"$PSScriptRoot\Dependencies\nlohmann" "$PSScriptRoot\tests\performance_metrics.cpp" "$PSScriptRoot\Deadlock_DMA\Diagnostics\Diagnostics.cpp" /Fe:"$output\performance-metrics.exe"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($performanceCommandPath, $performanceCommand, [Text.Encoding]::Default)
& $env:ComSpec /d /c $performanceCommandPath
if ($LASTEXITCODE -ne 0) { throw 'Performance metrics fixture compilation failed.' }
$performanceRun = Join-Path $output ('performance-' + [Guid]::NewGuid().ToString('N'))
& (Join-Path $output 'performance-metrics.exe') $performanceRun
if ($LASTEXITCODE -ne 0) { throw 'Performance metrics fixture failed.' }
