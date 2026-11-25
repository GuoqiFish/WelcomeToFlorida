param(
    [ValidateRange(1, 300)][int]$TimeoutSeconds = 20,
    [string[]]$BreakRva = @('0x1e1cfe0', '0x1e1cfe5', '0x24784d5', '0x247853a', '0x2478553',
        '0x24785b7', '0x2478698', '0x24786b9', '0x24786dc', '0x247870d', '0x2478731',
        '0x2478758', '0x247878a', '0x5a91bdd'),
    [string[]]$ExtraDll = @()
)
$ErrorActionPreference = 'Stop'
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
$manifest = [ordered]@{
    RunId = $runId; StartedUtc = [DateTime]::UtcNow.ToString('o'); Executable = $exe
    OriginalExeSha256 = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    TimeoutSeconds = $TimeoutSeconds; BreakRva = $BreakRva
    StorageLog = $storageLog; DeployedDlls = @()
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
    & $debugger $exe $log $TimeoutSeconds @BreakRva
    $manifest.DebuggerExitCode = $LASTEXITCODE
} catch {
    $manifest.Error = $_.Exception.Message
    throw
} finally {
    try {
        [Environment]::SetEnvironmentVariable('COMPAT_STORAGE_LOG', $previousStorageLog, 'Process')
    } catch { $cleanupErrors.Add("COMPAT_STORAGE_LOG restore failed: $($_.Exception.Message)") }
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
    $manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $run 'result.json') -Encoding UTF8
    Write-Output "Probe log: $log"
    Write-Output "Result: $(Join-Path $run 'result.json')"
    if ($cleanupErrors.Count) { Write-Warning ($cleanupErrors -join '; ') }
}
