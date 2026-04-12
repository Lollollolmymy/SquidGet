@echo off
REM ══════════════════════════════════════════════════════════════════════════
REM  squidget.bat  —  build and run SquidGet (Windows / TCC)
REM ══════════════════════════════════════════════════════════════════════════
setlocal enabledelayedexpansion
cd /d "%~dp0"

REM ── locate source root ────────────────────────────────────────────────────
:find_repo
if exist "main.c"                    goto found_repo
if exist "SquidGet-main\main.c"      ( cd SquidGet-main   & goto found_repo )
if exist "SquidGet\main.c"           ( cd SquidGet         & goto found_repo )
if exist "..\main.c"                 ( cd ..               & goto found_repo )
if exist "..\SquidGet-main\main.c"   ( cd ..\SquidGet-main & goto found_repo )

echo.
echo  ERROR: Cannot find SquidGet source files.
echo  Make sure squidget.bat is inside the SquidGet folder (next to main.c).
echo.
pause
exit /b 1

:found_repo
echo.
echo  SquidGet builder
echo  Source dir : %cd%
echo.

REM ── regenerate winhttp import library ─────────────────────────────────────
echo  [1/3] generating winhttp.def ...
tcc\tiny_impdef.exe C:\Windows\System32\winhttp.dll -o tcc\lib\winhttp.def
if errorlevel 1 (
    echo  WARNING: tiny_impdef failed (winhttp.def may be stale)
)

REM ── compile ───────────────────────────────────────────────────────────────
echo  [2/3] compiling ...
tcc\tcc.exe -O2 ^
    main.c api.c download.c ^
    tui.c json.c config.c platform.c tag.c ^
    tcc\lib\winhttp.def ^
    -I. -I"tcc\include" -I"tcc\include\winapi" ^
    -L"tcc\lib" ^
    -lshell32 -lole32 ^
    -o squidget.exe

if errorlevel 1 (
    echo.
    echo  *** COMPILATION FAILED ***
    echo  Check the errors above, then press any key to close.
    echo.
    pause
    exit /b 1
)
echo  [2/3] compilation successful.

REM ── run ───────────────────────────────────────────────────────────────────
echo  [3/3] launching squidget ...
echo.

squidget.exe
set EXIT_CODE=%errorlevel%

echo.
if %EXIT_CODE% NEQ 0 (
    echo  *** squidget exited with code %EXIT_CODE% ***
) else (
    echo  squidget exited normally.
)

echo.
pause
endlocal
