$ErrorActionPreference = 'Stop'
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$sources = @(
    @{ Source = 'placeholder_selftest.c'; Output = 'placeholder_selftest.exe' },
    @{ Source = 'placeholder_replace_selftest.c'; Output = 'placeholder_replace_selftest.exe' },
    @{ Source = 'xmem_placeholder_selftest.c'; Output = 'xmem_placeholder_selftest.exe' }
)

foreach ($test in $sources) {
    $sourcePath = Join-Path $PSScriptRoot $test.Source
    $outputPath = Join-Path (Join-Path $PSScriptRoot 'bin') $test.Output
    & $clang /nologo /W4 /O2 ("/Fe" + $outputPath) $sourcePath /link kernel32.lib
    if ($LASTEXITCODE -ne 0) { throw "Failed to build $($test.Source)" }
    if ($test.Output -eq 'xmem_placeholder_selftest.exe') {
        $dllPath = Join-Path (Join-Path $PSScriptRoot 'bin') 'xmem.dll'
        if (-not (Test-Path -LiteralPath $dllPath)) { throw 'Build xmem.dll before running the shim test.' }
        & $outputPath $dllPath
    } else {
        & $outputPath
    }
    if ($LASTEXITCODE -ne 0) { throw "Failed: $($test.Output)" }
}
