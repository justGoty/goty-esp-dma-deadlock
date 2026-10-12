[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools not found.' }
$vcvars = Join-Path $installation 'VC\Auxiliary\Build\vcvars64.bat'
$output = Join-Path $PSScriptRoot 'diagnostics\global-patterns'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$cmdPath = Join-Path $output 'compile.cmd'
# Compile and exercise fixed byte fixtures only, without VMM, DMA, graphics or input DLLs.
$command = @"
@echo off
call "$vcvars" >nul
if errorlevel 1 exit /b 1
cd /d "$output"
cl /nologo /std:c++20 /EHsc /utf-8 /W4 /WX /I"$PSScriptRoot\Deadlock_DMA" "$PSScriptRoot\tests\global_patterns_tests.cpp" /Fe:"$output\global-pattern-tests.exe"
exit /b %errorlevel%
"@
[IO.File]::WriteAllText($cmdPath, $command, [Text.Encoding]::Default)
& $env:ComSpec /d /c $cmdPath
if ($LASTEXITCODE -ne 0) { throw 'Global-pattern tests compilation failed.' }
& (Join-Path $output 'global-pattern-tests.exe')
if ($LASTEXITCODE -ne 0) { throw 'Global-pattern tests failed.' }
$global:LASTEXITCODE = 0
