param(
    [switch]$TranslateObservedAllocationFlags,
    [switch]$EnableObservedCreateCommittedResource3,
    [switch]$EnableObservedTextureArenaHook,
    [switch]$EnableObservedBvhPipelineBridge
)
$ErrorActionPreference = 'Stop'
if ($EnableObservedTextureArenaHook -and -not $EnableObservedCreateCommittedResource3) {
    throw 'EnableObservedTextureArenaHook requires the committed texture adapter.'
}

$gameRoot = Split-Path -Parent $PSScriptRoot
$runner = Join-Path $PSScriptRoot 'run-probe.ps1'
if (Get-Process -Name game_scarlett_master -ErrorAction SilentlyContinue) {
    throw 'A game instance is already running; the drive mappings have not been changed.'
}

$gameMapping = Get-SmbMapping -LocalPath 'G:' -ErrorAction SilentlyContinue
if (-not $gameMapping -or $gameMapping.RemotePath -notmatch '^\\\\') {
    throw 'G: must be an existing SMB mapping before the temporary game mapping can start.'
}
if ($gameMapping.GlobalMapping) {
    throw 'G: is a global SMB mapping; this per-user temporary remap will not modify it.'
}
if (Get-PSDrive -Name 'T' -ErrorAction SilentlyContinue) {
    throw 'T: is already in use; the original G: mapping has not been changed.'
}
if (Get-SmbMapping -LocalPath 'T:' -ErrorAction SilentlyContinue) {
    throw 'T: already has an SMB mapping; the original G: mapping has not been changed.'
}
$tConnection = & net.exe use T: 2>$null
if ($LASTEXITCODE -eq 0) {
    throw 'T: already has a network connection; the original G: mapping has not been changed.'
}
if ((& subst.exe) -match '(?im)^\s*T:') {
    throw 'T: is already a SUBST drive; the original G: mapping has not been changed.'
}
if (-not (Test-Path -LiteralPath (Join-Path $gameRoot 'game_scarlett_master.exe'))) {
    throw "Missing game executable under $gameRoot"
}

$networkRoot = $gameMapping.RemotePath
$persistent = Test-Path -LiteralPath 'HKCU:\Network\G'
$mappingOptions = @{
    RemotePath = $networkRoot
    RequireIntegrity = [bool]$gameMapping.RequireIntegrity
    RequirePrivacy = [bool]$gameMapping.RequirePrivacy
    UseWriteThrough = [bool]$gameMapping.UseWriteThrough
    CompressNetworkTraffic = [bool]$gameMapping.CompressNetworkTraffic
    BlockNTLM = [bool]$gameMapping.BlockNTLM
    SkipCertificateCheck = [bool]$gameMapping.SkipCertificateCheck
    TcpPort = [uint16]$gameMapping.TcpPort
    QuicPort = [uint16]$gameMapping.QuicPort
    RdmaPort = [uint16]$gameMapping.RdmaPort
}
if ($gameMapping.TransportType -ne 0) {
    $mappingOptions.TransportType = $gameMapping.TransportType
}
function Test-CapturedSmbMapping($candidate) {
    if (-not $candidate) { return $false }
    foreach ($property in @(
        'RemotePath', 'RequireIntegrity', 'RequirePrivacy', 'UseWriteThrough',
        'CompressNetworkTraffic', 'BlockNTLM', 'SkipCertificateCheck',
        'TcpPort', 'QuicPort', 'RdmaPort', 'TransportType'
    )) {
        if ($candidate.$property -ne $gameMapping.$property) { return $false }
    }
    return $true
}
$tMapped = $false
$gSubstituted = $false
$gRestored = $false
$cleanupErrors = [Collections.Generic.List[string]]::new()
$probeExitCode = $null

try {
    $tOptions = $mappingOptions.Clone()
    $tOptions.LocalPath = 'T:'
    $tOptions.Persistent = $false
    New-SmbMapping @tOptions -ErrorAction Stop | Out-Null
    $tMapped = $true

    Remove-SmbMapping -LocalPath 'G:' -Force -ErrorAction Stop

    & subst.exe G: $gameRoot
    if ($LASTEXITCODE -ne 0) { throw "Could not map the game directory to G: (SUBST exit $LASTEXITCODE)." }
    $gSubstituted = $true

    & $runner -TimeoutSeconds 60 -NoopXboxCommandListExtensions -NoopXboxDeviceSlot278 `
        -UseUnifiedXmemReservation -SkipObservedXmemPlaceholderSplits `
        -EnableObservedXmemBufferResource -ClampObservedOversizedDescriptorHeap `
        -NoopXboxDeviceSlot70 -TranslateObservedAllocationFlags:$TranslateObservedAllocationFlags `
        -EnableObservedCreateCommittedResource3:$EnableObservedCreateCommittedResource3 `
        -EnableObservedTextureArenaHook:$EnableObservedTextureArenaHook `
        -EnableObservedBvhPipelineBridge:$EnableObservedBvhPipelineBridge `
        -FocusStartupVm -ContinueObservedDescriptorHeapMismatch
    $probeExitCode = $LASTEXITCODE
} finally {
    if ($gSubstituted) {
        & subst.exe G: /d | Out-Null
        if ($LASTEXITCODE -ne 0) {
            $cleanupErrors.Add("Could not remove temporary G: SUBST mapping (exit $LASTEXITCODE).")
        }
    }
    $gCandidate = Get-SmbMapping -LocalPath 'G:' -ErrorAction SilentlyContinue
    if (Test-CapturedSmbMapping $gCandidate) {
        $gRestored = $true
    } elseif (-not $gCandidate) {
        try {
            $gOptions = $mappingOptions.Clone()
            $gOptions.LocalPath = 'G:'
            $gOptions.Persistent = $persistent
            New-SmbMapping @gOptions -ErrorAction Stop | Out-Null
            $restoredMapping = Get-SmbMapping -LocalPath 'G:' -ErrorAction Stop
            $gRestored = Test-CapturedSmbMapping $restoredMapping
        } catch {
            $cleanupErrors.Add("Could not verify restoration of G:: $($_.Exception.Message)")
        }
    } else {
        $cleanupErrors.Add("G: is occupied by an unexpected SMB mapping to $($gCandidate.RemotePath).")
    }
    if ($gRestored -and ((Test-Path -LiteralPath 'HKCU:\Network\G') -ne $persistent)) {
        $gRestored = $false
        $cleanupErrors.Add('G: SMB mapping was restored, but its persistence setting differs from the original.')
    }
    if ($tMapped -and $gRestored) {
        try {
            Remove-SmbMapping -LocalPath 'T:' -Force -ErrorAction Stop
            if (Get-SmbMapping -LocalPath 'T:' -ErrorAction SilentlyContinue) {
                $cleanupErrors.Add('T: still has an SMB mapping after removal.')
            }
        } catch {
            $cleanupErrors.Add("Could not remove temporary T: mapping: $($_.Exception.Message)")
        }
    } elseif ($tMapped) {
        $cleanupErrors.Add('Kept T: mapped to the original share because G: restoration could not be verified.')
    }
}

Write-Output "Probe debugger exit code: $probeExitCode"
if ($cleanupErrors.Count) { throw ($cleanupErrors -join ' ') }
Write-Output "Restored G: to $networkRoot with its captured SMB options and removed temporary T:."
