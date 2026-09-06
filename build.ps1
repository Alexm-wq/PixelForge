param(
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [switch]$Run
)
$ErrorActionPreference = 'Stop'
$buildDirectory = Join-Path $PSScriptRoot 'build'
& cmake -S $PSScriptRoot -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 -DBUILD_TESTING=ON
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& cmake --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
& ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$executable = Join-Path $buildDirectory "$Configuration/PixelForge.exe"
Write-Output "Built and tested: $executable"
if ($Run) { Start-Process -FilePath $executable -WindowStyle Normal }
