@echo off
REM Regenerate the recompiled sources (generated/default) from the manifest +
REM hook TOMLs. Run after editing config/hooks/*.toml. Hand edits in generated/
REM are LOST: re-apply the permanent fixes afterwards
REM   python local\diag\patch_missed_regs.py
REM   python local\diag\patch_scanguard.py
REM (patch_fix_ge2.py is NOT a fix - it introduced the state clamp; never run it.)
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
set "PATH=C:\Program Files\LLVM\bin;C:\Users\renoi\AppData\Local\Programs\Python\Python312\Scripts;%PATH%"
cd /d "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\build\win-amd64-Release"
echo === ninja ng2_codegen ===
ninja ng2_codegen
echo === EXITCODE %ERRORLEVEL% ===
