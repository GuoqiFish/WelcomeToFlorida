param(
    [ValidateRange(1, 300)][int]$TimeoutSeconds = 20,
    [string[]]$BreakRva = @('0x1e1cfe0', '0x1e1cfe5', '0x24784d5', '0x247853a', '0x2478553',
        '0x24785b7', '0x2478698', '0x24786b9', '0x24786dc', '0x247870d', '0x2478731',
        '0x2478758', '0x247878a', '0x2478b96', '0x2478ba4', '0x2478bb7', '0x2478bc5', '0x2478bcd', '0x2478bd0',
        '0x2478de4', '0x2478dea',
        '0x2478e22', '0x2478e27', '0x2478e29', '0x2478e2f', '0x24787a7',
        '0x19f4f54', '0x19f4f56',
        '0x247abd8', '0x247abde',
        '0x2236e00', '0x2236e6e', '0x2236e73', '0x223716c', '0x5a91bdd', '0x1fb1f50', '0x1fb1f78',
        '0x56eb30', '0x56eb3f', '0x56eb5e', '0x56eb74', '0x56eb7e', '0x56eba8', '0x56ebbd', '0x56ebd6', '0x56ebf0',
        'D3D12Core.dll!0x237528', 'D3D12Core.dll!0x237539', 'D3D12Core.dll!0x11b0c3'),
    [string[]]$ExtraDll = @(),
    [switch]$EnableD3D12Debug,
    [switch]$NoopXboxCommandListExtensions,
    [switch]$NoopXboxDeviceSlot278,
    [switch]$AllowObservedXmemVaReservation,
    [switch]$UseUnifiedXmemReservation,
    [switch]$SkipObservedXmemPlaceholderSplits,
    [switch]$EnableObservedXmemBufferResource,
    [switch]$ClampObservedOversizedDescriptorHeap,
    [switch]$NoopXboxDeviceSlot70,
    [switch]$TranslateObservedAllocationFlags,
    [switch]$EnableObservedCreateCommittedResource3,
    [switch]$EnableObservedTextureArenaHook,
    [switch]$EnableObservedBvhPipelineBridge,
    [switch]$FocusStartupVm,
    [switch]$ContinueObservedDescriptorHeapMismatch
)
$ErrorActionPreference = 'Stop'
if ($UseUnifiedXmemReservation -xor $SkipObservedXmemPlaceholderSplits) {
    throw 'UseUnifiedXmemReservation and SkipObservedXmemPlaceholderSplits must be enabled together.'
}
if ($EnableObservedXmemBufferResource -and -not $UseUnifiedXmemReservation) {
    throw 'EnableObservedXmemBufferResource requires the unified XMem reservation mode.'
}
if ($EnableObservedTextureArenaHook -and -not $EnableObservedCreateCommittedResource3) {
    throw 'EnableObservedTextureArenaHook requires the committed texture adapter.'
}
if ($FocusStartupVm) {
    # Leave room for the four call sites required by unified XMem mode.
    $BreakRva = @(
        '0x1e1cfe0', '0x1e1cfe5',
        '0x193dff8', '0x193e1b4', '0x193e1ba', '0x193e2d4',
        '0x247ad64', '0x247ad69', '0x247aeb6', '0x247af25',
        '0x56a2a60', '0x56a2a8b', '0x56a2abe',
        '0x247af38', '0x2479430', '0x247945c', '0x19f4f59',
        '0x247944f', '0x2479455', '0x247954e', '0x2479554',
        '0x1e864d3', '0x1e86500',
        '0x1e86537', '0x1e86564',
        '0x1e86600', '0x1e86605',
        '0x1e86630', '0x1e86635',
        '0x1e8666c', '0x1e86671', '0x4deb4eb',
        '0x1e86709',
        '0x1e86723', '0x1e8672e',
        '0x1e86738', '0x1e8673d',
        '0x1e86760', '0x1e86765',
        '0x1e867d1', '0x1e867d6',
        '0x1e8684c', '0x1e86853', '0x1e86868', '0x1e8686e',
        '0x1e868c2', '0x1e868eb',
        '0x2478e22', '0x2478e27', '0x2478e29', '0x2478e2f', '0x24787a7',
        '0x19f4f54',
        '0x1cf3355', '0x1cf335b', '0x1cf336a', '0x1cf338f', '0x1cf3395',
        '0x344182', '0x344188'
    )
}
if ($SkipObservedXmemPlaceholderSplits) {
    $requiredSplits = @('0x1e864cd', '0x1e864fa', '0x1e86531', '0x1e8655e')
    $BreakRva = $requiredSplits + @($BreakRva | Where-Object { $_ -notin $requiredSplits })
}
if ($BreakRva.Count -gt 64) {
    throw "The debugger supports 64 breakpoints; $($BreakRva.Count) were requested."
}
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $PSScriptRoot 'bin'
$exe = Join-Path $root 'game_scarlett_master.exe'
$debugger = Join-Path $bin 'probe_debugger.exe'
$dlls = @('xmem.dll', 'd3d12_xs.dll', 'chad.dll', 'xg_xs.dll', 'XCRDAPI.dll', 'dstorage.dll', 'dstoragecore.dll', 'xapu.dll', 'AcpHal.dll') + $ExtraDll
$dllNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($dll in $dlls) {
    if ([IO.Path]::GetFileName($dll) -ne $dll -or $dll -notmatch '\.dll$') { throw "Invalid DLL filename: $dll" }
    if (-not $dllNames.Add($dll)) { throw "Duplicate DLL filename: $dll" }
}
if (@(Get-Process -Name game_scarlett_master -ErrorAction SilentlyContinue).Count) {
    throw 'An existing game instance is running. Close it before deploying probe DLLs.'
}
foreach ($file in $dlls + @('probe_debugger.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $bin $file))) { throw "Build the probe first; missing $file" }
}
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N').Substring(0, 8)
$run = Join-Path $PSScriptRoot ('runs\' + $runId)
$backup = Join-Path $run 'backup'
New-Item -ItemType Directory -Path $backup -Force | Out-Null
$log = Join-Path $run 'debug.log'
$storageLog = Join-Path $run 'storage.log'
$previousStorageLog = [Environment]::GetEnvironmentVariable('COMPAT_STORAGE_LOG', 'Process')
$previousD3D12Debug = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_DEBUG', 'Process')
$previousCommandListNoop = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_XBOX_COMMAND_LIST_NOOP_EXTENSIONS', 'Process')
$previousDeviceSlot278Noop = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_XBOX_DEVICE_SLOT278_NOOP', 'Process')
$previousDeviceSlot70Noop = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_XBOX_DEVICE_SLOT70_NOOP', 'Process')
$previousAllocationFlagsTranslation = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_TRANSLATE_OBSERVED_ALLOCATION_FLAGS', 'Process')
$previousCommittedResource3Adapter = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_XBOX_SLOT3F8_COMMITTED_RESOURCE3', 'Process')
$previousTextureArenaHook = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_XBOX_TEXTURE_ARENA_HOOK', 'Process')
$previousBvhPipelineBridge = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_XBOX_BVH_PIPELINES', 'Process')
$previousXmemVaReservation = [Environment]::GetEnvironmentVariable('COMPAT_XMEM_EXACT_VA_RESERVATION', 'Process')
$previousUnifiedXmemReservation = [Environment]::GetEnvironmentVariable('COMPAT_XMEM_UNIFIED_RESERVATION', 'Process')
$previousSkipXmemPlaceholderSplits = [Environment]::GetEnvironmentVariable('COMPAT_SKIP_XMEM_PLACEHOLDER_SPLITS', 'Process')
$previousXmemBufferResource = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_XBOX_SLOT428_BUFFER', 'Process')
$previousDescriptorHeapClamp = [Environment]::GetEnvironmentVariable('COMPAT_D3D12_CLAMP_OBSERVED_DESCRIPTOR_HEAP', 'Process')
$previousDescriptorHeapMismatchContinue = [Environment]::GetEnvironmentVariable('COMPAT_CONTINUE_OBSERVED_DESCRIPTOR_HEAP_MISMATCH', 'Process')
$manifest = [ordered]@{
    RunId = $runId; StartedUtc = [DateTime]::UtcNow.ToString('o'); Executable = $exe
    OriginalExeSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    TimeoutSeconds = $TimeoutSeconds; BreakRva = $BreakRva
    StorageLog = $storageLog; WindowCapture = ($log + '.window.bmp')
    D3D12DebugEnabled = $EnableD3D12Debug.IsPresent
    XboxCommandListExtensionNoopsEnabled = $NoopXboxCommandListExtensions.IsPresent
    XboxDeviceSlot278NoopEnabled = $NoopXboxDeviceSlot278.IsPresent
    XboxDeviceSlot70NoopEnabled = $NoopXboxDeviceSlot70.IsPresent
    ObservedAllocationFlagsTranslationEnabled = $TranslateObservedAllocationFlags.IsPresent
    ObservedCreateCommittedResource3AdapterEnabled = $EnableObservedCreateCommittedResource3.IsPresent
    ObservedTextureArenaHookEnabled = $EnableObservedTextureArenaHook.IsPresent
    ObservedBvhPipelineBridgeEnabled = $EnableObservedBvhPipelineBridge.IsPresent
    ObservedXmemVaReservationEnabled = $AllowObservedXmemVaReservation.IsPresent
    UnifiedXmemReservationEnabled = $UseUnifiedXmemReservation.IsPresent
    SkippedObservedXmemPlaceholderSplits = $SkipObservedXmemPlaceholderSplits.IsPresent
    ObservedXmemBufferResourceEnabled = $EnableObservedXmemBufferResource.IsPresent
    ObservedOversizedDescriptorHeapClampEnabled = $ClampObservedOversizedDescriptorHeap.IsPresent
    ObservedDescriptorHeapMismatchContinueEnabled = $ContinueObservedDescriptorHeapMismatch.IsPresent
    FocusedStartupVmTraceEnabled = $FocusStartupVm.IsPresent
    DeployedDlls = @()
    DebuggerExitCode = $null; CleanupSucceeded = $false; RestoredFiles = @(); Error = $null
}
$deployed = [Collections.Generic.List[string]]::new()
$existing = @{}
$cleanupErrors = [Collections.Generic.List[string]]::new()
try {
    foreach ($dll in $dlls) {
        $destination = Join-Path $root $dll
        if (Test-Path -LiteralPath $destination) {
            $existing[$dll] = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
            Copy-Item -LiteralPath $destination -Destination (Join-Path $backup $dll)
        }
        $deployed.Add($dll)
        Copy-Item -LiteralPath (Join-Path $bin $dll) -Destination $destination -Force
        $manifest.DeployedDlls += [ordered]@{
            Name = $dll; Sha256 = (Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash
        }
    }
    [Environment]::SetEnvironmentVariable('COMPAT_STORAGE_LOG', $storageLog, 'Process')
    $debugFlag = if ($EnableD3D12Debug) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable('COMPAT_D3D12_DEBUG', $debugFlag, 'Process')
    $commandListNoopFlag = if ($NoopXboxCommandListExtensions) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_D3D12_XBOX_COMMAND_LIST_NOOP_EXTENSIONS', $commandListNoopFlag, 'Process')
    $deviceSlot278NoopFlag = if ($NoopXboxDeviceSlot278) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_D3D12_XBOX_DEVICE_SLOT278_NOOP', $deviceSlot278NoopFlag, 'Process')
    $deviceSlot70NoopFlag = if ($NoopXboxDeviceSlot70) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_D3D12_XBOX_DEVICE_SLOT70_NOOP', $deviceSlot70NoopFlag, 'Process')
    $allocationFlagsTranslation = if ($TranslateObservedAllocationFlags) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_D3D12_TRANSLATE_OBSERVED_ALLOCATION_FLAGS', $allocationFlagsTranslation, 'Process')
    $committedResource3Adapter = if ($EnableObservedCreateCommittedResource3) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_D3D12_XBOX_SLOT3F8_COMMITTED_RESOURCE3', $committedResource3Adapter, 'Process')
    $textureArenaHook = if ($EnableObservedTextureArenaHook) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable('COMPAT_D3D12_XBOX_TEXTURE_ARENA_HOOK', $textureArenaHook, 'Process')
    $bvhBridge = if ($EnableObservedBvhPipelineBridge) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable('COMPAT_D3D12_XBOX_BVH_PIPELINES', $bvhBridge, 'Process')
    $xmemVaReservationFlag = if ($AllowObservedXmemVaReservation) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_XMEM_EXACT_VA_RESERVATION', $xmemVaReservationFlag, 'Process')
    $unifiedXmemFlag = if ($UseUnifiedXmemReservation) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_XMEM_UNIFIED_RESERVATION', $unifiedXmemFlag, 'Process')
    $skipXmemSplitsFlag = if ($SkipObservedXmemPlaceholderSplits) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_SKIP_XMEM_PLACEHOLDER_SPLITS', $skipXmemSplitsFlag, 'Process')
    $xmemBufferResourceFlag = if ($EnableObservedXmemBufferResource) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_D3D12_XBOX_SLOT428_BUFFER', $xmemBufferResourceFlag, 'Process')
    $descriptorHeapClampFlag = if ($ClampObservedOversizedDescriptorHeap) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_D3D12_CLAMP_OBSERVED_DESCRIPTOR_HEAP', $descriptorHeapClampFlag, 'Process')
    $descriptorHeapMismatchContinueFlag = if ($ContinueObservedDescriptorHeapMismatch) { '1' } else { '0' }
    [Environment]::SetEnvironmentVariable(
        'COMPAT_CONTINUE_OBSERVED_DESCRIPTOR_HEAP_MISMATCH', $descriptorHeapMismatchContinueFlag, 'Process')
    & $debugger $exe $log $TimeoutSeconds @BreakRva
    $manifest.DebuggerExitCode = $LASTEXITCODE
} catch {
    $manifest.Error = $_.Exception.Message
    throw
} finally {
    try {
        [Environment]::SetEnvironmentVariable('COMPAT_STORAGE_LOG', $previousStorageLog, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_STORAGE_LOG restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable('COMPAT_D3D12_DEBUG', $previousD3D12Debug, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_DEBUG restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_D3D12_XBOX_COMMAND_LIST_NOOP_EXTENSIONS', $previousCommandListNoop, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_XBOX_COMMAND_LIST_NOOP_EXTENSIONS restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_D3D12_XBOX_DEVICE_SLOT278_NOOP', $previousDeviceSlot278Noop, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_XBOX_DEVICE_SLOT278_NOOP restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_D3D12_XBOX_DEVICE_SLOT70_NOOP', $previousDeviceSlot70Noop, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_XBOX_DEVICE_SLOT70_NOOP restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_D3D12_TRANSLATE_OBSERVED_ALLOCATION_FLAGS', $previousAllocationFlagsTranslation, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_TRANSLATE_OBSERVED_ALLOCATION_FLAGS restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_D3D12_XBOX_SLOT3F8_COMMITTED_RESOURCE3', $previousCommittedResource3Adapter, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_XBOX_SLOT3F8_COMMITTED_RESOURCE3 restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_D3D12_XBOX_TEXTURE_ARENA_HOOK', $previousTextureArenaHook, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_XBOX_TEXTURE_ARENA_HOOK restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_D3D12_XBOX_BVH_PIPELINES', $previousBvhPipelineBridge, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_XBOX_BVH_PIPELINES restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_XMEM_EXACT_VA_RESERVATION', $previousXmemVaReservation, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_XMEM_EXACT_VA_RESERVATION restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_XMEM_UNIFIED_RESERVATION', $previousUnifiedXmemReservation, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_XMEM_UNIFIED_RESERVATION restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_SKIP_XMEM_PLACEHOLDER_SPLITS', $previousSkipXmemPlaceholderSplits, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_SKIP_XMEM_PLACEHOLDER_SPLITS restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_D3D12_XBOX_SLOT428_BUFFER', $previousXmemBufferResource, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_XBOX_SLOT428_BUFFER restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_D3D12_CLAMP_OBSERVED_DESCRIPTOR_HEAP', $previousDescriptorHeapClamp, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_D3D12_CLAMP_OBSERVED_DESCRIPTOR_HEAP restore failed: $($_.Exception.Message)") }
    try {
        [Environment]::SetEnvironmentVariable(
            'COMPAT_CONTINUE_OBSERVED_DESCRIPTOR_HEAP_MISMATCH', $previousDescriptorHeapMismatchContinue, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_CONTINUE_OBSERVED_DESCRIPTOR_HEAP_MISMATCH restore failed: $($_.Exception.Message)") }
    foreach ($dll in $deployed) {
        try {
            $destination = Join-Path $root $dll
            if ($existing.ContainsKey($dll)) {
                Copy-Item -LiteralPath (Join-Path $backup $dll) -Destination $destination -Force
                if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $existing[$dll]) {
                    throw "Restored hash mismatch: $dll"
                }
            } elseif (Test-Path -LiteralPath $destination) {
                Remove-Item -LiteralPath $destination -Force
            }
            $manifest.RestoredFiles += $dll
        } catch { $cleanupErrors.Add($_.Exception.Message) }
    }
    $manifest.CleanupSucceeded = $cleanupErrors.Count -eq 0
    $manifest['CleanupErrors'] = @($cleanupErrors.ToArray())
    $manifest['ExecutableUnchanged'] = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash -eq $manifest.OriginalExeSha256
    $manifest['FinishedUtc'] = [DateTime]::UtcNow.ToString('o')
    $manifest['StorageLogExists'] = Test-Path -LiteralPath $storageLog -PathType Leaf
    $manifest['WindowCaptureExists'] = Test-Path -LiteralPath $manifest.WindowCapture -PathType Leaf
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $run 'result.json') -Encoding UTF8
    Write-Output "Probe log: $log"
    Write-Output "Result: $(Join-Path $run 'result.json')"
    if ($cleanupErrors.Count) { Write-Warning ($cleanupErrors -join '; ') }
}
