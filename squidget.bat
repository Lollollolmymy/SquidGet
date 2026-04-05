@echo off
setlocal enabledelayedexpansion

:: get absolute path regardless of how the script was called
cd /d "%~dp0"
set "DIR=%cd%\"
set "TCC=%DIR%tcc\tcc.exe"
set "IMPDEF=%DIR%tcc\tiny_impdef.exe"
set "EXE=%DIR%squidget.exe"

:: already compiled: just run
if exist "%EXE%" goto run

echo [squidget] compiling...

:: Generate winhttp.def from the system DLL to ensure compatibility 
"%IMPDEF%" C:\Windows\System32\winhttp.dll -o "%DIR%tcc\lib\winhttp.def"

:: Compile using the .def file directly to avoid the TCC search order bug 
:: To enable API response logging, change 0 to 1 on the next line:
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