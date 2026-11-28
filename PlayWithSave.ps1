# SPDX-License-Identifier: Unlicense

[CmdletBinding()]
param(
    [string]$DataDirectory = (Join-Path $env:LOCALAPPDATA 'WTFVI/Data'),
    [string]$GameDirectory,
    [string]$StreamlineDirectory
)

$ErrorActionPreference = 'Stop'
$PSBoundParameters['DataDirectory'] = $DataDirectory

if (!$PSBoundParameters.ContainsKey('StreamlineDirectory') -and !$env:VICTORY_STREAMLINE_DIR) {
    $sdk = Join-Path $env:LOCALAPPDATA 'WelcomeToFlorida/nvidia/sdk/Streamline/bin/x64'
    $validationSdk = Join-Path $PSScriptRoot '../Florida-render-validation/nvidia-dlss-10fps/runtime/streamline'
    $requiredDlls = 'sl.interposer.dll', 'sl.common.dll', 'sl.dlss.dll', 'nvngx_dlss.dll'
    foreach ($directory in $sdk, $validationSdk) {
        $missingDlls = $requiredDlls | Where-Object {
            !(Test-Path -LiteralPath (Join-Path $directory $_) -PathType Leaf)
        }
        if (!$missingDlls) {
            $PSBoundParameters['StreamlineDirectory'] = $directory
            break
        }
    }
}

& (Join-Path $PSScriptRoot 'Play-SeriesS-DLSS.ps1') @PSBoundParameters
exit $LASTEXITCODE
