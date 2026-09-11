@echo off
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64 >nul
set "PATH=C:\Program Files\LLVM\bin;C:\Users\renoi\AppData\Local\Programs\Python\Python312\Scripts;%PATH%"
cd /d "C:\Users\renoi\ClaudeCode\Ninja Gaiden 2 Xbox360\ng2recomp\out\build\win-amd64-Release"
echo === ninja ng2.exe ===
ninja ng2.exe
echo === EXITCODE %ERRORLEVEL% ===
