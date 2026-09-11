@echo off
setlocal
rem Sample a RUNNING ng2: start it, let it settle, attach cdb and dump every
rem thread's stack, then detach and stop it by PID.
rem
rem   tools\sample.cmd [seconds]
rem
rem Use this when the game is alive but not progressing - a crash trace tells
rem you where it died, this tells you where it is stuck.

set "PROJECT_ROOT=%~dp0.."
set "BUILD_DIR=%PROJECT_ROOT%\out\build\win-amd64-RelWithDebInfo"
set "CDB=C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"
set "_NT_SYMBOL_PATH=%BUILD_DIR%"
set "WAIT=%~1"
if "%WAIT%"=="" set "WAIT=25"

if not exist "%BUILD_DIR%\ng2.exe" ( echo ERROR: build RelWithDebInfo first & exit /b 1 )

pushd "%PROJECT_ROOT%"
start "" /b "%BUILD_DIR%\ng2.exe" --game_data_root game --log_file out\sample.log --log_level debug
popd

powershell -NoProfile -Command "Start-Sleep -Seconds %WAIT%"

for /f %%p in ('powershell -NoProfile -Command "(Get-Process ng2 -ErrorAction SilentlyContinue | Select-Object -First 1).Id"') do set PID=%%p
if "%PID%"=="" ( echo ERROR: ng2 is not running & exit /b 1 )
echo attaching to PID %PID%

rem -pv attaches non-invasively so detaching cannot kill the process.
"%CDB%" -pv -p %PID% -c ".lines -e; ~*k 14; q"

powershell -NoProfile -Command "Stop-Process -Id %PID% -Force -ErrorAction SilentlyContinue"
