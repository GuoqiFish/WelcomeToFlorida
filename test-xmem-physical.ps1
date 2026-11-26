$ErrorActionPreference = 'Stop'
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
$selftest = Join-Path $PSScriptRoot 'bin\xmem_physical_selftest.exe'
$dll = Join-Path $PSScriptRoot 'bin\xmem.dll'
& $clang /nologo /O2 /DWIN32_LEAN_AND_MEAN ("/Fe" + $selftest) (Join-Path $PSScriptRoot 'xmem_physical_selftest.c')
if ($LASTEXITCODE -ne 0) { throw 'xmem physical-page self-test build failed' }
& $selftest $dll
if ($LASTEXITCODE -ne 0) { throw 'xmem physical-page self-test failed' }
