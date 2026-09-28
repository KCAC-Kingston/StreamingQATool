$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$BuildSpec = Get-Content (Join-Path $ProjectRoot 'buildspec.json') -Raw | ConvertFrom-Json
$QtRoot = Join-Path $ProjectRoot ".deps/obs-deps-qt6-$($BuildSpec.dependencies.qt6.version)-x64"
$DepsRoot = Join-Path $ProjectRoot ".deps/obs-deps-$($BuildSpec.dependencies.prebuilt.version)-x64"
$TestBuild = Join-Path $ProjectRoot 'build_tests'
$PreviousPath = $env:PATH
$PreviousPluginPath = $env:QT_PLUGIN_PATH
try {
    & cmake -S $PSScriptRoot -B $TestBuild -G 'Visual Studio 17 2022' -A x64 "-DCMAKE_PREFIX_PATH=$QtRoot"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    & cmake --build $TestBuild --config Release --parallel
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    $env:PATH = "$QtRoot/bin;$DepsRoot/bin;$PreviousPath"
    $env:QT_PLUGIN_PATH = "$QtRoot/plugins"
    & ctest --test-dir $TestBuild -C Release --output-on-failure
    exit $LASTEXITCODE
} finally {
    $env:PATH = $PreviousPath
    $env:QT_PLUGIN_PATH = $PreviousPluginPath
}
