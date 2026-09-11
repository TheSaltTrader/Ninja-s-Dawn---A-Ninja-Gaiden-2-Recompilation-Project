@echo off
REM Compile only patch_hooks.cpp, without linking - the C++ can be checked while
REM ng2.exe is still running and the linker could not replace it.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
ninja -C out\build\win-amd64-Release CMakeFiles\ng2.dir\src\patch_hooks.cpp.obj
