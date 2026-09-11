@echo off
setlocal
rem Run ng2 under cdb and record a native stack for every access violation.
rem
rem   tools\crash_trace.cmd [RelWithDebInfo|Debug] > out\crash.txt
rem
rem Recompiled guest functions are real native functions named sub_<guest
rem address>, so a host stack frame names the guest function directly - which
rem is the fastest way to turn "guest access violation at 0x1A4" into "guest
rem function 0x836302E0 dereferenced null".
rem
rem The runtime also emulates guest MMIO through access violations, so benign
rem AVs are expected; each is passed back to the app with `gn` (go not
rem handled) and the interesting one is the last stack before the process dies.

set "PROJECT_ROOT=%~dp0.."
set "BUILD_TYPE=%~1"
if "%BUILD_TYPE%"=="" set "BUILD_TYPE=RelWithDebInfo"
set "BUILD_DIR=%PROJECT_ROOT%\out\build\win-amd64-%BUILD_TYPE%"
set "CDB=C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"

if not exist "%CDB%" ( echo ERROR: cdb.exe not found & exit /b 1 )
if not exist "%BUILD_DIR%\ng2.exe" ( echo ERROR: build %BUILD_TYPE% first & exit /b 1 )

set "_NT_SYMBOL_PATH=%BUILD_DIR%"

rem -g skips the loader breakpoint, so the first prompt cdb offers is the
rem faulting exception itself - dump the stack there and quit. (Setting an
rem `sxe -c` handler from the initial command is too late: the initial command
rem is only read once cdb already has a prompt, i.e. after the fault.)
rem Line tables map the faulting host address back to the generated
rem ng2_recomp.<n>.cpp line, and codegen emits one line per guest instruction -
rem so `lsa` names the exact guest instruction that faulted.
"%CDB%" -g -G -o -c ".lines -e; r; ln @rip; .echo === SOURCE ===; lsa @rip; .echo === STACK ===; kv 40; q" ^
    "%BUILD_DIR%\ng2.exe" ^
    --game_data_root "%PROJECT_ROOT%\game" ^
    --log_file "%PROJECT_ROOT%\out\crash_run.log" ^
    --log_level debug
