@echo off
setlocal enabledelayedexpansion

REM ── Locate vcvarsall ─────────────────────────────────────────────────────
set VCVARS=
if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" (
    set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
)
if not defined VCVARS (
    for %%V in (2022 2019 2017) do (
        for %%E in (Community Professional Enterprise BuildTools) do (
            if exist "C:\Program Files\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat" (
                if not defined VCVARS set "VCVARS=C:\Program Files\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat"
            )
            if exist "C:\Program Files (x86)\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat" (
                if not defined VCVARS set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\%%V\%%E\VC\Auxiliary\Build\vcvarsall.bat"
            )
        )
    )
)
if not defined VCVARS (
    echo ERROR: Visual Studio vcvarsall.bat not found.
    pause & exit /b 1
)

set CMAKE=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe
set NINJA=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe

call "%VCVARS%" x64

if not exist build_cmake mkdir build_cmake

echo.
echo === CONFIGURE ===
"%CMAKE%" -S . -B build_cmake -G "Ninja" -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%NINJA%"
if %errorlevel% neq 0 (
    echo.
    echo ==============================
    echo   CONFIGURE FAILED
    echo ==============================
    pause & exit /b 1
)

echo.
echo === BUILD ===
"%CMAKE%" --build "%~dp0build_cmake" --config Release -j4
if %errorlevel% equ 0 (
    echo.
    echo ==============================
    echo   BUILD SUCCESS
    echo   build_cmake\KOG25DEditor.exe
    echo ==============================
) else (
    echo.
    echo ==============================
    echo   BUILD FAILED
    echo ==============================
)

pause
