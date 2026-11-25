$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$out = Join-Path $PSScriptRoot 'bin'
$clang = 'C:\Program Files\LLVM\bin\clang-cl.exe'
New-Item -ItemType Directory -Force -Path $out | Out-Null

& $clang /nologo /LD /O2 /DWIN32_LEAN_AND_MEAN ("/Fe" + (Join-Path $out 'xmem.dll')) (Join-Path $PSScriptRoot 'xbox_stubs.c') /link ("/DEF:" + (Join-Path $PSScriptRoot 'xmem.def'))
if ($LASTEXITCODE -ne 0) { throw "xmem.dll build failed" }

$dlls = @('d3d12_xs.dll','chad.dll','xg_xs.dll','xapu.dll','AcpHal.dll')
foreach ($dll in $dlls) {
    $def = Join-Path $PSScriptRoot ($dll + '.def')
    @("LIBRARY $dll", 'EXPORTS', '    D3D12ConfigureVirtualMemoryX', '    D3D12SerializeVersionedRootSignature', '    D3D12XboxCreateDevice', '    XGCreateTextureComputer', '    XGSuggestSwizzleMode', '    DStorageGetFactory', '    XDspActivate', '    XDspConnectWithMaximumStreamLimit', '    XDspDeactivate', '    XDspDisconnect', '    XDspSubmitCommand', '    XApuConnect', '    XApuDequeueResult', '    XApuDisconnect', '    XApuEnqueueCommand', '    XApuGetResultCount', '    AcpHalAllocateShapeContexts', '    AcpHalCreate', '    AcpHalReleaseShapeContexts', '    ApuAlloc', '    ApuCreateHeap', '    ApuFree', '    ApuIsVirtualAddressValid', '    ApuMapApuAddress', '    ApuMapVirtualAddress', '    GameInputCreate') | Set-Content -LiteralPath $def -Encoding ASCII
    & $clang /nologo /LD /O2 /DWIN32_LEAN_AND_MEAN ("/Fe" + (Join-Path $out $dll)) (Join-Path $PSScriptRoot 'xbox_stubs.c') /link ("/DEF:" + $def)
    if ($LASTEXITCODE -ne 0) { throw "$dll build failed" }
}

$gameInputDef = Join-Path $PSScriptRoot 'gameinput.def'
& $clang /nologo /LD /O2 /DWIN32_LEAN_AND_MEAN ("/Fe" + (Join-Path $out 'GameInput.dll')) (Join-Path $PSScriptRoot 'xbox_stubs.c') /link ("/DEF:" + $gameInputDef)
if ($LASTEXITCODE -ne 0) { throw "GameInput.dll build failed" }

& (Join-Path $PSScriptRoot 'build-storage.ps1')
Write-Output "Built compatibility probe DLLs in $out"
