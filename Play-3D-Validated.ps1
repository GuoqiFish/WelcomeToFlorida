# SPDX-License-Identifier: Unlicense

[CmdletBinding()]
param(
    [string]$GameDirectory = (Join-Path $PSScriptRoot '..\GTA6.Xbox.v1.0.2.1-CYBERLEEK'),
    [string]$ValidationDirectory = (Join-Path $PSScriptRoot '..\Florida-render-validation\20261127-133203-series-s-full-textures')
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

$gamePath = (Resolve-Path -LiteralPath $GameDirectory).ProviderPath
$launchArguments = @($gamePath)
$runPath = $ValidationDirectory
if ($PSBoundParameters.ContainsKey('ValidationDirectory')) {
    $runPath = (Resolve-Path -LiteralPath $ValidationDirectory).ProviderPath
    $launchArguments += Join-Path $runPath 'portable'
}

$launchEnvironment = @{
    VICTORY_WAVE64_PAIRED = '1'
    VICTORY_LEVELS_ON_DEMAND = '0'
    VICTORY_BVH_BUILDER = '0'
    VICTORY_WINDOW_MAX = '960x540'
    VICTORY_D3D12_DRED = 'light'
}
if (Test-Path -LiteralPath $runPath -PathType Container) {
    if (-not $env:VICTORY_CACHE_DIR) {
        $launchEnvironment.VICTORY_CACHE_DIR = Join-Path $runPath 'cache'
    }
    if (-not $env:VICTORY_DATA_DIR) {
        $launchEnvironment.VICTORY_DATA_DIR = Join-Path $runPath 'data'
    }
}

$previousEnvironment = @{}
try {
    foreach ($setting in $launchEnvironment.GetEnumerator()) {
        $previousEnvironment[$setting.Key] = [Environment]::GetEnvironmentVariable($setting.Key, 'Process')
        [Environment]::SetEnvironmentVariable($setting.Key, $setting.Value, 'Process')
    }

    & (Join-Path $PSScriptRoot 'Play-3D.cmd') @launchArguments
    $launchExitCode = $LASTEXITCODE
} finally {
    foreach ($setting in $previousEnvironment.GetEnumerator()) {
        if ($null -eq $setting.Value) {
            [Environment]::SetEnvironmentVariable($setting.Key, [NullString]::Value, 'Process')
        } else {
            [Environment]::SetEnvironmentVariable($setting.Key, $setting.Value, 'Process')
        }
    }
}

exit $launchExitCode
