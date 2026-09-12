@echo off
REM Compile the app's own translation units WITHOUT linking, so the C++ can be
REM checked while ng2.exe is running and the linker could not replace it.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
set "PATH=C:\Program Files\LLVM\bin;C:\Users\renoi\AppData\Local\Programs\Python\Python312\Scripts;%PATH%"
cd /d "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\build\win-amd64-Release"
ninja CMakeFiles/ng2.dir/src/ng2_menu.cpp.obj CMakeFiles/ng2.dir/src/main.cpp.obj CMakeFiles/ng2.dir/src/ng2_saveimport.cpp.obj CMakeFiles/ng2.dir/src/ng2_disc.cpp.obj CMakeFiles/ng2.dir/src/ng2_diagnostics.cpp.obj
echo === EXITCODE %ERRORLEVEL% ===
