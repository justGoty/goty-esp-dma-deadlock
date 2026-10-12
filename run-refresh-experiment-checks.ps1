[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools not found.' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $PSScriptRoot 'diagnostics\refresh-experiment'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$commandPath = Join-Path $output 'compile.cmd'
$command = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$output"
cl /nologo /std:c++latest /EHsc /utf-8 /W4 /wd4200 /I"$PSScriptRoot\Deadlock_DMA" /I"$PSScriptRoot\Dependencies\MemProcFS" "$PSScriptRoot\tests\refresh_experiment.cpp" /Fe:"$output\refresh-experiment-tests.exe"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($commandPath, $command, [Text.Encoding]::Default)
& $env:ComSpec /d /c $commandPath
if ($LASTEXITCODE -ne 0) { throw 'Refresh experiment fixture compilation failed.' }
& (Join-Path $output 'refresh-experiment-tests.exe') | Tee-Object -FilePath (Join-Path $output 'results.txt')
if ($LASTEXITCODE -ne 0) { throw 'Refresh experiment fixture failed.' }
