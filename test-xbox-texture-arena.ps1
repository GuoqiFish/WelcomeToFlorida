param(
    [string]$SdkVersion = '10.0.26100.0'
)
$ErrorActionPreference = 'Stop'
$compat = $PSScriptRoot
$sdkRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10'
$sdkInclude = Join-Path $sdkRoot (Join-Path 'Include' $SdkVersion)
$sdkLib = Join-Path $sdkRoot (Join-Path 'Lib' (Join-Path $SdkVersion 'um\x64'))
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
if (-not (Test-Path -LiteralPath $clang)) { throw "clang-cl not found: $clang" }
if (-not (Test-Path -LiteralPath (Join-Path $sdkInclude 'um\d3d12.h'))) {
    throw "Windows SDK headers not found for $SdkVersion"
}
if (-not (Test-Path -LiteralPath (Join-Path $sdkLib 'd3d12.lib'))) {
    throw "Windows SDK libraries not found for $SdkVersion"
}

$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-texture-arena-selftest'
$run = Join-Path $compat (Join-Path 'runs' $runId)
New-Item -ItemType Directory -Path $run -Force | Out-Null
$exe = Join-Path $run 'xbox_texture_arena_selftest.exe'
$log = Join-Path $run 'selftest.log'
& $clang /nologo /std:c++17 /EHsc /MD /W4 /O2 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH `
    (('/I' + (Join-Path $sdkInclude 'shared'))) `
    (('/I' + (Join-Path $sdkInclude 'um'))) `
    (('/I' + (Join-Path $sdkInclude 'ucrt'))) `
    (Join-Path $compat 'xbox_texture_arena_selftest.cpp') `
    ('/Fe' + $exe) `
    /link (('/LIBPATH:' + $sdkLib)) d3d12.lib dxgi.lib dxguid.lib
if ($LASTEXITCODE -ne 0) { throw 'Texture arena self-test build failed' }

& $exe 2>&1 | Tee-Object -FilePath $log
$exitCode = $LASTEXITCODE
@{
    RunId = $runId
    Executable = $exe
    Log = $log
    ExitCode = $exitCode
    FinishedLocal = (Get-Date).ToString('o')
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'result.json') -Encoding UTF8
if ($exitCode -ne 0) { throw "Texture arena self-test failed with exit code $exitCode; see $log" }
Write-Output "Self-test log: $log"
