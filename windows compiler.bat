@echo off
REM Universal SquidGet Builder - works from any downloaded version
setlocal enabledelayedexpansion
cd /d "%~dp0"

:find_repo
if exist "main.c" goto found_repo
if exist "SquidGet\main.c" (
    cd SquidGet
    goto found_repo
)
if exist "SquidGet-main\main.c" (
    cd SquidGet-main
    goto found_repo
)
if exist ".\SquidGet-main\main.c" (
    cd .\SquidGet-main
    goto found_repo
)
if exist "..\main.c" (
    cd ..
    goto found_repo
)
if exist "..\..\main.c" (
    cd ..\..
    goto found_repo
)

echo Error: Could not find SquidGet source files in:
echo Current: %cd%
echo Checking parent directories...
if exist "..\SquidGet-main\main.c" (
    cd ..\SquidGet-main
    goto found_repo
)

pause
exit /b 1

:found_repo
echo [squidget] compiling...

tcc\tiny_impdef.exe C:\Windows\System32\winhttp.dll -o tcc\lib\winhttp.def

set SQT_DEBUG=1
if "%SQT_DEBUG%"=="1" (set DBG_FLAG=-DSQT_DEBUG) else (set DBG_FLAG=)

tcc\tcc.exe -O2 %DBG_FLAG% ^
    main.c api.c download.c ^
    tui.c json.c config.c platform.c ^
    tcc\lib\winhttp.def ^
    -I. -I"tcc\include" -I"tcc\include\winapi" ^
    -L"tcc\lib" ^
    -lshell32 -lole32 ^
    -o squidget.exe

if errorlevel 1 (
    if exist squidget.exe del squidget.exe
    echo [squidget] compilation failed.
    pause
    exit /b 1
)

echo [squidget] done.

:run
squidget.exe
