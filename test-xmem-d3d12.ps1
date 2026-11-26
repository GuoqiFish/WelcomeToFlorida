$ErrorActionPreference = 'Stop'
$out = Join-Path $PSScriptRoot 'bin'
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
New-Item -ItemType Directory -Force -Path $out | Out-Null

& $clang /nologo /EHsc /std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /W4 /O2 `
    (Join-Path $PSScriptRoot 'xmem_d3d12_selftest.cpp') `
    ("/Fe" + (Join-Path $out 'xmem_d3d12_selftest.exe')) `
    /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'D3D12 XMem bridge self-test build failed' }

& (Join-Path $out 'xmem_d3d12_selftest.exe')
if ($LASTEXITCODE -ne 0) { throw "D3D12 XMem bridge self-test failed: $LASTEXITCODE" }

& (Join-Path $out 'xmem_d3d12_selftest.exe') topology
if ($LASTEXITCODE -eq 3) {
    Write-Output 'The XMem placeholder topology is not a single placeable heap on this host.'
} elseif ($LASTEXITCODE -ne 0) {
    throw "D3D12 XMem placeholder-topology test failed: $LASTEXITCODE"
}

& (Join-Path $out 'xmem_d3d12_selftest.exe') unified
if ($LASTEXITCODE -ne 0) { throw "D3D12 unified-reservation test failed: $LASTEXITCODE" }

$slotTest = Join-Path $out 'xbox_slot428_selftest.exe'
& $clang /nologo /EHsc /std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /W4 /O2 `
    (Join-Path $PSScriptRoot 'xbox_slot428_selftest.cpp') `
    ("/Fe" + $slotTest) /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Xbox slot 0x428 self-test build failed' }
& $slotTest (Join-Path $out 'd3d12_xs.dll')
if ($LASTEXITCODE -ne 0) { throw "Xbox slot 0x428 self-test failed: $LASTEXITCODE" }

$heapAbiTest = Join-Path $out 'xbox_descriptor_heap_abi_selftest.exe'
& $clang /nologo /EHsc /std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /W4 /O2 `
    (Join-Path $PSScriptRoot 'xbox_descriptor_heap_abi_selftest.cpp') `
    ("/Fe" + $heapAbiTest) /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Xbox descriptor-heap ABI self-test build failed' }
& $heapAbiTest (Join-Path $out 'd3d12_xs.dll')
if ($LASTEXITCODE -ne 0) { throw "Xbox descriptor-heap ABI self-test failed: $LASTEXITCODE" }

& $heapAbiTest (Join-Path $out 'd3d12_xs.dll') '--oversized-retry'
if ($LASTEXITCODE -ne 0) { throw "Xbox oversized descriptor-heap retry self-test failed: $LASTEXITCODE" }

& $heapAbiTest (Join-Path $out 'd3d12_xs.dll') '--oversized-near-miss'
if ($LASTEXITCODE -ne 0) { throw "Xbox oversized descriptor-heap near-miss self-test failed: $LASTEXITCODE" }

$allocationInfoTest = Join-Path $out 'xbox_allocation_info_selftest.exe'
& $clang /nologo /EHsc /std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /W4 /O2 `
    (Join-Path $PSScriptRoot 'xbox_allocation_info_selftest.cpp') `
    ("/Fe" + $allocationInfoTest) /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Xbox allocation-info ABI self-test build failed' }
& $allocationInfoTest (Join-Path $out 'd3d12_xs.dll')
if ($LASTEXITCODE -ne 0) { throw "Xbox allocation-info ABI self-test failed: $LASTEXITCODE" }

$committedResource3Test = Join-Path $out 'xbox_committed_resource3_selftest.exe'
& $clang /nologo /EHsc /std:c++17 /D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH /W4 /O2 `
    (Join-Path $PSScriptRoot 'xbox_committed_resource3_selftest.cpp') `
    ("/Fe" + $committedResource3Test) /link d3d12.lib dxgi.lib
if ($LASTEXITCODE -ne 0) { throw 'Xbox CreateCommittedResource3 self-test build failed' }
& $committedResource3Test (Join-Path $out 'd3d12_xs.dll')
if ($LASTEXITCODE -ne 0) { throw "Xbox CreateCommittedResource3 self-test failed: $LASTEXITCODE" }
