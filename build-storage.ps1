$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot 'bin'
$vendor = Join-Path $PSScriptRoot 'vendor\directstorage-1.3.0\native'
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $clang /nologo /std:c++17 /EHsc /MD /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /W4 /Zi /O2 /LD `
    ('/I' + (Join-Path $vendor 'include')) `
    (Join-Path $PSScriptRoot 'storage_compat.cpp') `
    ('/Fo' + (Join-Path $out 'storage_compat.obj')) `
    ('/Fe' + (Join-Path $out 'XCRDAPI.dll')) `
    /link ('/DEF:' + (Join-Path $PSScriptRoot 'storage_compat.def')) `
    ('/PDB:' + (Join-Path $out 'storage_compat.pdb')) ole32.lib
if ($LASTEXITCODE -ne 0) { throw 'Storage adapter build failed' }
foreach ($dll in @('dstorage.dll', 'dstoragecore.dll')) {
    $source = Join-Path $vendor ('bin\x64\' + $dll)
    $signature = Get-AuthenticodeSignature -LiteralPath $source
    if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=Microsoft Corporation') {
        throw "Official runtime signature validation failed: $dll"
    }
    Copy-Item -LiteralPath $source -Destination (Join-Path $out $dll) -Force
}
Write-Output "Built DirectStorage adapter in $out"
