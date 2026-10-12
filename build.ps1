[CmdletBinding()]
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release', [switch]$Rebuild)
$ErrorActionPreference = 'Stop'
$repoRoot = $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install Visual Studio 2022 C++ Build Tools (v143, Windows SDK).' }
$installation = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $installation) { throw 'Visual Studio C++ tools were not found.' }
$msbuild = Join-Path $installation 'MSBuild\Current\Bin\MSBuild.exe'
$output = Join-Path $repoRoot 'diagnostics'
New-Item -ItemType Directory -Path $output -Force | Out-Null
Push-Location $repoRoot
try {
    $buildTarget = if ($Rebuild) { '/t:Rebuild' } else { '/t:Build' }
    & $msbuild (Join-Path $repoRoot 'Deadlock_DMA.sln') $buildTarget /m "/p:Configuration=$Configuration" /p:Platform=x64 /p:PlatformToolset=v143 /nologo /verbosity:minimal /fl "/flp:logfile=$output\build-$Configuration.log"
    if ($LASTEXITCODE -ne 0) { throw "Build failed. See $output\build-$Configuration.log" }
    $runtime = Join-Path $repoRoot "x64\$Configuration"
    $resources = Join-Path $repoRoot 'Resources'
    if (Test-Path -LiteralPath $resources -PathType Container) {
        Copy-Item -LiteralPath $resources -Destination $runtime -Recurse -Force
    } else {
        Write-Host 'Optional Resources assets are not included in this source-only snapshot.'
    }
    foreach ($symbolFile in @('dbghelp.dll', 'symsrv.dll', 'info.db')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "Dependencies\MemProcFS\$symbolFile") -Destination $runtime -Force
    }
    foreach ($notice in @('LICENSE-MemProcFS.txt', 'license_dbghelp.txt', 'redist_dbghelp.txt', 'license_info_all.txt')) {
        Copy-Item -LiteralPath (Join-Path $repoRoot "Dependencies\MemProcFS\$notice") -Destination $runtime -Force
    }
    Write-Host "Built: $runtime\GOTY-Deadlock-DMA.exe"
} finally { Pop-Location }
