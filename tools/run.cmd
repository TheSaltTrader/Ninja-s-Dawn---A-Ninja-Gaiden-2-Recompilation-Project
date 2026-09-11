@echo off
setlocal
rem Launch ng2 against the extracted disc in game\.
rem
rem   tools\run.cmd [extra ng2 args...]
rem
rem The app also defaults game_data_root to game\ beside the executable
rem (see Ng2App::OnConfigurePaths), so a junction in the build directory lets
rem you launch ng2.exe directly. This script sets it explicitly either way.

set "PROJECT_ROOT=%~dp0.."
set "BUILD_TYPE=Release"
set "EXE=%PROJECT_ROOT%\out\build\win-amd64-%BUILD_TYPE%\ng2.exe"

if not exist "%EXE%" (
    echo ERROR: %EXE% not found - run tools\build.cmd first
    exit /b 1
)

"%EXE%" --game_data_root "%PROJECT_ROOT%\game" ^
        --log_file "%PROJECT_ROOT%\out\ng2.log" ^
        --log_level debug ^
        %*
