@echo off
setlocal EnableDelayedExpansion
rem Configure and build ng2.
rem
rem   tools\build.cmd [Release|Debug|RelWithDebInfo] [extra cmake args...]
rem
rem clang++ targets x86_64-pc-windows-msvc, so it needs the MSVC INCLUDE/LIB
rem environment that vcvars64 sets. Everything else the project links (SDL3,
rem fmt, spdlog, utf8cpp, the Xenos GPU plugin) ships inside the ReXGlue SDK,
rem so there is no vcpkg dependency for a stock project.

set "PROJECT_ROOT=%~dp0.."
set "SDK_DIR=%PROJECT_ROOT%\..\RexBlue\win-amd64"
set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
set "LLVM_BIN=C:\Program Files\LLVM\bin"
set "NINJA_BIN=%LOCALAPPDATA%\Programs\Python\Python312\Scripts"

set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=Release"

rem `shift` does not rewrite %*, so collect the remaining arguments by hand -
rem otherwise the build type is passed to CMake again as a stray path.
set "EXTRA_ARGS="
:collect_args
shift
if "%~1"=="" goto args_collected
set "EXTRA_ARGS=%EXTRA_ARGS% %1"
goto collect_args
:args_collected

set "BUILD_DIR=%PROJECT_ROOT%\out\build\win-amd64-%BUILD_TYPE%"

rem Release drops debug info for the ~560 recompiled translation units (that is
rem a lot of PDB for code nobody reads). Any other config keeps line tables, so
rem a crash resolves to a sub_<guest address> frame under a debugger.
if /I "%BUILD_TYPE%"=="Release" (
    set "RECOMP_DEBUG_INFO=none"
) else (
    set "RECOMP_DEBUG_INFO=line-tables-only"
)

if not exist "%VCVARS%" (
    echo ERROR: vcvars64.bat not found at "%VCVARS%"
    exit /b 1
)
call "%VCVARS%" >nul || exit /b 1

set "PATH=%LLVM_BIN%;%NINJA_BIN%;%PATH%"

where clang++ >nul 2>&1 || (echo ERROR: clang++ not on PATH & exit /b 1)
where ninja   >nul 2>&1 || (echo ERROR: ninja not on PATH & exit /b 1)

rem Run codegen before CMake rather than letting the in-build custom command do
rem it. Codegen rewrites generated\default\ng2_pch.h, and inside a single ninja
rem run the precompiled header can be built from the old copy before codegen
rem replaces it - every TU then fails with "file has been modified since the
rem precompiled header was built". Generating first makes the headers final
rem before anything compiles.
echo === Codegen ===
"%SDK_DIR%\bin\rexglue.exe" codegen "%PROJECT_ROOT%\ng2_manifest.toml" || exit /b 1

echo === Configuring (%BUILD_TYPE%) ===
cmake -S "%PROJECT_ROOT%" -B "%BUILD_DIR%" -G Ninja ^
    -DCMAKE_BUILD_TYPE=%BUILD_TYPE% ^
    -DCMAKE_C_COMPILER=clang ^
    -DCMAKE_CXX_COMPILER=clang++ ^
    -DCMAKE_PREFIX_PATH="%SDK_DIR%" ^
    -DREXGLUE_RECOMP_DEBUG_INFO=%RECOMP_DEBUG_INFO% ^
    %EXTRA_ARGS% || exit /b 1

echo === Building ===
cmake --build "%BUILD_DIR%" || exit /b 1

echo === Done: %BUILD_DIR% ===
