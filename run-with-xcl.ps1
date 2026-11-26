param(
    [Parameter(Mandatory)][string]$XclExe,
    [ValidateRange(1, 3600)][int]$RunSeconds = 360,
    [ValidateRange(2, 120)][int]$CaptureInterval = 15,
    [ValidateRange(0.1, 3600.0)][double[]]$PressAAt
)
$ErrorActionPreference = 'Stop'
$title = Split-Path -Parent $PSScriptRoot
$gameExe = (Resolve-Path -LiteralPath (Join-Path $title 'game_scarlett_master.exe')).Path
$xcl = (Resolve-Path -LiteralPath $XclExe).Path
if (Get-Process -Name xcl,game_scarlett_master,probe_debugger -ErrorAction SilentlyContinue) {
    throw 'Close the existing game or XCL run before launching another one.'
}

# XCL's guest VFS redirects paths below the game directory. Put TLS carriers,
# logs, and captures outside it so the Win32 loader can open them directly.
$runId = (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)
$runPath = [IO.Path]::GetFullPath((Join-Path ([IO.Path]::GetTempPath()) "scarlett-xcl-$runId"))
if ($runPath.StartsWith($title.TrimEnd('\', '/') + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'The system temporary directory must be outside the game directory.'
}
New-Item -ItemType Directory -Path $runPath | Out-Null
$environment = [ordered]@{
    XCL_DATA_ROOT = Join-Path $runPath 'data'
    XCL_NETWORK = '0'
    XCL_FRAME_DUMP = [string]$CaptureInterval
    XCL_RUN_SECONDS = [string]$RunSeconds
    XCL_SKIP_BINK = $null
    XCL_PRESS_AT = $null
    XCL_PRESS_A_AT = if ($PressAAt) {
        ($PressAAt | ForEach-Object { $_.ToString([Globalization.CultureInfo]::InvariantCulture) }) -join ','
    } else { $null }
}
$savedEnvironment = @{}
$manifest = [ordered]@{
    RunId = $runId
    StartedLocal = (Get-Date).ToString('o')
    XclExecutable = $xcl
    XclSha256 = (Get-FileHash -LiteralPath $xcl).Hash
    OriginalExeSha256 = (Get-FileHash -LiteralPath $gameExe).Hash
    DataRoot = $environment.XCL_DATA_ROOT
    CaptureInterval = $CaptureInterval
    PressAAt = $PressAAt
    ExitCode = $null
}
Write-Output "XCL artifacts: $runPath"
try {
    foreach ($entry in $environment.GetEnumerator()) {
        $savedEnvironment[$entry.Key] = [Environment]::GetEnvironmentVariable($entry.Key, 'Process')
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
    & $xcl run $gameExe 1> (Join-Path $runPath 'stdout.log') 2> (Join-Path $runPath 'stderr.log')
    $manifest.ExitCode = $LASTEXITCODE
} finally {
    foreach ($entry in $savedEnvironment.GetEnumerator()) {
        [Environment]::SetEnvironmentVariable($entry.Key, $entry.Value, 'Process')
    }
    $manifest.ExecutableUnchanged = (Get-FileHash -LiteralPath $gameExe).Hash -eq $manifest.OriginalExeSha256
    $manifest.FinishedLocal = (Get-Date).ToString('o')
    $manifest | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runPath 'result.json') -Encoding UTF8
}
Write-Output "XCL exit: $($manifest.ExitCode); executable unchanged: $($manifest.ExecutableUnchanged)"
