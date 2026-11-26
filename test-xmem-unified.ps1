$ErrorActionPreference = 'Stop'
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$out = Join-Path $PSScriptRoot 'bin'
$test = Join-Path $out 'xmem_unified_selftest.exe'

& $clang /nologo /W4 /O2 ("/Fe" + $test) `
    (Join-Path $PSScriptRoot 'xmem_unified_selftest.c') /link kernel32.lib
if ($LASTEXITCODE -ne 0) { throw 'Unified XMem self-test build failed' }

$dll = Join-Path $out 'xmem.dll'
if (-not (Test-Path -LiteralPath $dll)) { throw 'Build xmem.dll before running the unified test.' }
& $test $dll
if ($LASTEXITCODE -ne 0) { throw "Unified XMem self-test failed: $LASTEXITCODE" }
