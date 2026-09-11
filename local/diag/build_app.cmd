@echo off
REM Build ng2.exe.
REM
REM vcvars is required (without it clang falls back to MSVC 19.33 and the 19.44
REM PCH fails to load), and LLVM must be on PATH: the cached build.ninja holds
REM full compiler paths, so this only bites when something forces CMake to
REM re-configure - which is exactly when it is most confusing.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
cmake --build out/build/win-amd64-Release --config Release --target ng2
REM NOTE: this step re-copies STOCK SDK DLLs from RexBlue. Redeploy after.
