# SPDX-License-Identifier: Unlicense

[CmdletBinding()]
param(
    [string]$GameDirectory = (Join-Path $PSScriptRoot '../GTA6.Xbox.v1.0.2.1-CYBERLEEK'),
    [string]$StreamlineDirectory = $env:VICTORY_STREAMLINE_DIR,
    [string]$DataDirectory = (Join-Path $env:LOCALAPPDATA 'WelcomeToFlorida/nvidia/data'),
    [string]$CacheDirectory = (Join-Path $env:LOCALAPPDATA 'WelcomeToFlorida/nvidia/cache'),
    [switch]$WarmCache
)

$ErrorActionPreference = 'Stop'
$runtime = Join-Path $PSScriptRoot 'victory-nvidia/target/wtf-series-s'
$cli = Join-Path $runtime 'kelvaroq.exe'
$engine = Join-Path $runtime 'native/kelvaroq-engine.exe'
if (!(Test-Path -LiteralPath $cli -PathType Leaf) -or !(Test-Path -LiteralPath $engine -PathType Leaf)) {
    throw 'Run Build-SeriesS-DLSS.ps1 to build the NVIDIA backend and Kelvaroq CLI.'
}

$separator = [IO.Path]::DirectorySeparatorChar
$repo = [IO.Path]::GetFullPath($PSScriptRoot).TrimEnd($separator) + $separator
$data = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($DataDirectory)
$cache = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($CacheDirectory)
foreach ($path in $data, $cache) {
    if (([IO.Path]::GetFullPath($path).TrimEnd($separator) + $separator).StartsWith($repo, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Use a data or cache directory outside WelcomeToFlorida ($path)."
    }
}

if (!$StreamlineDirectory) {
    $StreamlineDirectory = Join-Path $env:LOCALAPPDATA 'WelcomeToFlorida/nvidia/sdk/Streamline/bin/x64'
}
if (!$WarmCache) {
    $game = Join-Path (Resolve-Path -LiteralPath $GameDirectory).ProviderPath 'game_scarlett_master.exe'
    if (!(Test-Path -LiteralPath $game -PathType Leaf)) { throw "Game executable missing at $game." }
    foreach ($dll in 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll') {
        if (!(Test-Path -LiteralPath (Join-Path $StreamlineDirectory $dll) -PathType Leaf)) {
            throw "Streamline file $dll is missing. See the Streamline setup in README.md."
        }
    }
    $StreamlineDirectory = (Resolve-Path -LiteralPath $StreamlineDirectory).ProviderPath
}

# Keep the settings from the measured 1080p Series S profile together with the launcher.
$profile = @{
    VICTORY_DATA_DIR = $data
    VICTORY_CACHE_DIR = $cache
    VICTORY_STREAMLINE_DIR = $StreamlineDirectory
    VICTORY_DISPLAY_MODE = '1920x1080'
    VICTORY_WINDOW_MAX = '1920x1080'
    VICTORY_DLSS = '1'
    VICTORY_DLSS_MODE = 'quality'
    VICTORY_FRAMEGEN = '0'
    VICTORY_STREAMLINE_PRESENT = '1'
    VICTORY_LOG = 'warn,title=info,victory_gpu::host=info,victory_gpu::streamline=info,victory_gpu::dxgi=info,victory_gpu::pipeline::library=info,victory_hle::modules::fidelityfx=info,victory_hle::modules::gameinput=info'
    VICTORY_BVH_BUILDER = '0'
    VICTORY_D3D12_DRED = 'faults'
    VICTORY_PIPELINE_BOOK = '1'
    VICTORY_THIN = '1'
    VICTORY_JOIN_PARTS = '8'
    VICTORY_RELAY = '2'
    VICTORY_RELAY_COPY = '1'
    VICTORY_WIDE_BACK = '1'
    VICTORY_CARD_REHASH_RUNS = '1'
    VICTORY_JOIN_COPIES = '1'
    VICTORY_CARD_STEPS = '2'
    VICTORY_CARD_AWAKE = '1'
    VICTORY_CARD_BUFFERS_MIN_KB = '64'
    VICTORY_INDIRECT_WINDOW = '1'
    VICTORY_INDIRECT_BATCH = '2'
    VICTORY_UPLOAD_AHEAD = '1'
    VICTORY_SHOW_AFTER = '1'
    VICTORY_MAP_THREADS = '1'
    VICTORY_WC_NO_BACK = '1'
    VICTORY_WC_NO_BACK_MIN_KB = '1024'
    VICTORY_SOFT_VIEWS = 'copies'
    VICTORY_TEXTURE_POOL = '1'
    VICTORY_RIPE_BLOCKS = '0'
    VICTORY_TEXTURE_NO_WAIT = '1'
    VICTORY_TEXTURE_CATCHUP = '0'
    VICTORY_TEXTURE_STAGE_MS = '16'
    VICTORY_LEVELS_ON_DEMAND = '0'
    VICTORY_PRELUDE_UPLOADS = '1'
    VICTORY_SHOW_NEW_LATER = '1'
    VICTORY_POOL_SMALL = '0'
    VICTORY_TABLED_REREAD = '1'
    VICTORY_QUEUE_HOST_TEXTURES = '0'
    VICTORY_HEAP_SYNC_FIRST = '1'
    VICTORY_SYNC_IN_PLACE = '1'
    VICTORY_SPANS_QUICK = '1'
    VICTORY_SYNC_FINE = '1'
    VICTORY_HEAP_SYNC_AT_SUBMIT = '1'
    VICTORY_AMBIENT = '0.8'
    VICTORY_AMBIENT_AIR = '1.5'
    VICTORY_AMBIENT_SPECULAR = '1'
    VICTORY_EARLY_DEPTH = '1'
}

$launch = [Diagnostics.ProcessStartInfo]::new()
$launch.UseShellExecute = $false
$launch.WorkingDirectory = Join-Path $runtime 'native'
if ($WarmCache) {
    $launch.FileName = $engine
    $launch.Arguments = 'warm --used --jobs 2'
} else {
    $launch.FileName = $cli
    $launch.Arguments = 'play --console series-s "{0}"' -f $game
}

# Prevent legacy renderer settings from changing this profile. Retain diagnostic inputs.
foreach ($key in @($launch.EnvironmentVariables.Keys)) {
    if ($key -match '^(VICTORY_|KELVAROQ_|RUST_LOG$)' -and $key -notin 'VICTORY_KEYS_FILE', 'VICTORY_FRAME_TRACE') {
        $launch.EnvironmentVariables.Remove($key)
    }
}
foreach ($setting in $profile.GetEnumerator()) {
    $launch.EnvironmentVariables[$setting.Key] = $setting.Value
}

$process = [Diagnostics.Process]::Start($launch)
try {
    $process.WaitForExit()
    $code = $process.ExitCode
} finally {
    $process.Dispose()
}
exit $code
