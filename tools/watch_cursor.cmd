@echo off
setlocal
set "PROJECT_ROOT=%~dp0.."
set "BUILD_DIR=%PROJECT_ROOT%\out\build\win-amd64-RelWithDebInfo"
set "CDB=C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"
set "_NT_SYMBOL_PATH=%BUILD_DIR%"
"%CDB%" -G -o -cf "%~dp0watch_cursor.cdb" "%BUILD_DIR%\ng2.exe" ^
    --game_data_root "%PROJECT_ROOT%\game" ^
    --log_file "%PROJECT_ROOT%\out\watch.log" --log_level debug
