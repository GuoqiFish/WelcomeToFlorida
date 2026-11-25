param(
    [string]$InputFile = (Join-Path (Split-Path -Parent $PSScriptRoot) 'xbs_splash.png')
)
$ErrorActionPreference = 'Stop'
$bin = Join-Path $PSScriptRoot 'bin'
$adapter = Join-Path $bin 'XCRDAPI.dll'
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$testExe = Join-Path $bin 'storage_selftest.exe'
$inputPath = (Resolve-Path -LiteralPath $InputFile).Path
foreach ($required in @($adapter, (Join-Path $bin 'dstorage.dll'), (Join-Path $bin 'dstoragecore.dll'))) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Build storage adapter first; missing $required" }
}
& $clang /nologo /W4 /std:c++17 /EHsc /MD /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /O2 `
    (Join-Path $PSScriptRoot 'storage_selftest.cpp') `
    ("/Fo" + (Join-Path $bin 'storage_selftest.obj')) `
    ("/Fe" + $testExe) /link ole32.lib
if ($LASTEXITCODE -ne 0) { throw 'Storage self-test build failed' }

$runId = 'storage-selftest-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$run = Join-Path $PSScriptRoot ('runs\' + $runId)
New-Item -ItemType Directory -Path $run | Out-Null
$adapterLog = Join-Path $run 'storage.log'
$testLog = Join-Path $run 'selftest.log'
$priorLog = [Environment]::GetEnvironmentVariable('COMPAT_STORAGE_LOG', 'Process')
$exitCode = $null
try {
    [Environment]::SetEnvironmentVariable('COMPAT_STORAGE_LOG', $adapterLog, 'Process')
    & $testExe $adapter $inputPath 2>&1 | Tee-Object -FilePath $testLog
    $exitCode = $LASTEXITCODE
} finally {
    [Environment]::SetEnvironmentVariable('COMPAT_STORAGE_LOG', $priorLog, 'Process')
    [ordered]@{
        Adapter = $adapter
        AdapterSha256 = (Get-FileHash -LiteralPath $adapter -Algorithm SHA256).Hash
        InputFile = $inputPath
        BytesComparedPerTransfer = 4096
        ExitCode = $exitCode
        Passed = $exitCode -eq 0
        AdapterLog = $adapterLog
        TestLog = $testLog
    } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run 'result.json') -Encoding UTF8
    Write-Output "Storage test evidence: $run"
}
if ($exitCode -ne 0) { throw "Storage self-test failed with exit code $exitCode" }
