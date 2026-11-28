# SPDX-License-Identifier: Unlicense

[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$renderer = Join-Path $PSScriptRoot 'victory-nvidia'
$kelvaroq = Join-Path $PSScriptRoot 'kelvaroq'
$rendererTarget = Join-Path $renderer 'target'
$kelvaroqTarget = Join-Path $kelvaroq 'target'

& cargo +1.98.1 build --locked --release --manifest-path (Join-Path $renderer 'Cargo.toml') --target-dir $rendererTarget --bin victory
if ($LASTEXITCODE -ne 0) { throw "VIctory-NVIDIA build failed ($LASTEXITCODE)." }

& cargo +1.98.1 build --locked --release --manifest-path (Join-Path $kelvaroq 'Cargo.toml') --target-dir $kelvaroqTarget --bin kelvaroq
if ($LASTEXITCODE -ne 0) { throw "Kelvaroq build failed ($LASTEXITCODE)." }

$package = Join-Path $rendererTarget 'wtf-series-s'
$native = Join-Path $package 'native'
New-Item -ItemType Directory -Path $native -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $rendererTarget 'release/victory.exe') -Destination (Join-Path $native 'kelvaroq-engine.exe') -Force
Copy-Item -LiteralPath (Join-Path $kelvaroqTarget 'release/kelvaroq.exe') -Destination (Join-Path $package 'kelvaroq.exe') -Force
$package
