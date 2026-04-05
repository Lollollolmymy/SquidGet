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
set "DIR=%cd%\"
set "TCC=%DIR%tcc\tcc.exe"
set "IMPDEF=%DIR%tcc\tiny_impdef.exe"
set "EXE=%DIR%squidget.exe"

if exist "%EXE%" (
    echo [squidget] launching...
    goto run
)

echo [squidget] compiling...

"%IMPDEF%" C:\Windows\System32\winhttp.dll -o "%DIR%tcc\lib\winhttp.def"

set SQT_DEBUG=1
if "%SQT_DEBUG%"=="1" (set DBG_FLAG=-DSQT_DEBUG) else (set DBG_FLAG=)

"%TCC%" -O2 %DBG_FLAG% ^
    "%DIR%main.c" "%DIR%api.c" "%DIR%download.c" ^
    "%DIR%tui.c"  "%DIR%json.c" "%DIR%config.c" "%DIR%platform.c" ^
    "%DIR%tcc\lib\winhttp.def" ^
    -I"%DIR%" -I"%DIR%tcc\include" -I"%DIR%tcc\include\winapi" ^
    -L"%DIR%tcc\lib" ^
    -lshell32 -lole32 -lm ^
    -o "%EXE%"

if errorlevel 1 (
    if exist "%EXE%" del "%EXE%"
    echo [squidget] compilation failed.
    pause
    exit /b 1
)

echo [squidget] done.

:run
"%EXE%"
