@echo off
REM Compile only the app objects that changed, without linking - so the C++ can
REM be checked while ng2.exe is running and the linker could not replace it.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp"
ninja -C out\build\win-amd64-Release CMakeFiles\ng2.dir\src\ng2_menu.cpp.obj CMakeFiles\ng2.dir\src\ng2_textool.cpp.obj
