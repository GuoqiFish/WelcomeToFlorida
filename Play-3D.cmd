@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
rem Adapted from cool_fox/Play-VIctory.cmd at f4b48ad. See cool_fox/LICENSE.
rem Modified 2026-11-26 to launch the Series S profile through Kelvaroq.
setlocal EnableExtensions DisableDelayedExpansion

if "%~1"=="" (
    echo Usage: Play-3D.cmd GAME_DIRECTORY [KELVAROQ_PORTABLE_DIRECTORY] 1>&2
    exit /b 2
)

set "VICTORY_GAME_DIR=%~f1"
set "gameImage=%VICTORY_GAME_DIR%\game_scarlett_master.exe"
set "kelvaroqDir=%~dp0kelvaroq\target\release"
set "kelvaroqCli=kelvaroq.exe"
if not "%~2"=="" (
    set "kelvaroqDir=%~f2"
    set "kelvaroqCli=kelvaroq-cli.exe"
)

if not exist "%gameImage%" (
    echo Game executable missing: "%gameImage%" 1>&2
    exit /b 2
)
if not exist "%kelvaroqDir%\%kelvaroqCli%" (
    echo Kelvaroq CLI missing: "%kelvaroqDir%\%kelvaroqCli%" 1>&2
    exit /b 2
)
if not exist "%kelvaroqDir%\native\kelvaroq-engine.exe" (
    echo Native backend missing: "%kelvaroqDir%\native\kelvaroq-engine.exe" 1>&2
    exit /b 2
)

if not defined VICTORY_DATA_DIR set "VICTORY_DATA_DIR=%LOCALAPPDATA%\WelcomeToFlorida\data"
if not defined VICTORY_CACHE_DIR set "VICTORY_CACHE_DIR=%LOCALAPPDATA%\WelcomeToFlorida\cache"
if not defined VICTORY_LOG if not defined RUST_LOG set "VICTORY_LOG=warn"

rem Apply cool_fox's pareto-residency profile to the Series S launch.
set "VICTORY_WAVE_WIDE="
set "VICTORY_AMBIENT_AIR=1.5"
if not defined VICTORY_TEXTURE_NO_WAIT set "VICTORY_TEXTURE_NO_WAIT=1"
if not defined VICTORY_RELAY set "VICTORY_RELAY=2"
set "VICTORY_WIDE_BACK=1"
set "VICTORY_CARD_REHASH_RUNS=1"
set "VICTORY_JOIN_COPIES=1"
if not defined VICTORY_INDIRECT_WINDOW set "VICTORY_INDIRECT_WINDOW=1"
set "VICTORY_RELAY_COPY=1"
if not defined VICTORY_CARD_STEPS set "VICTORY_CARD_STEPS=2"
set "VICTORY_INDIRECT_BATCH=2"
set "VICTORY_MAP_THREADS=1"
set "VICTORY_CARD_AWAKE=1"
set "VICTORY_SHOW_AFTER=1"
set "VICTORY_AMBIENT=0.8"
if not defined VICTORY_THIN set "VICTORY_THIN=1"
set "VICTORY_JOIN_PARTS=8"
set "VICTORY_UPLOAD_AHEAD=1"
if not defined VICTORY_LEVELS_ON_DEMAND set "VICTORY_LEVELS_ON_DEMAND=1"
set "VICTORY_CARD_BUFFERS_MIN_KB=64"

pushd "%VICTORY_GAME_DIR%" || exit /b 2
"%kelvaroqDir%\%kelvaroqCli%" play --console series-s "%gameImage%"
set "playExitCode=%errorlevel%"
popd
exit /b %playExitCode%
