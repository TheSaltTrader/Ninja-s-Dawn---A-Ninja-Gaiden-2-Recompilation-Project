@echo off
REM Build the SDK plugin + runtime pair. Deploy BOTH together: a source-built
REM plugin against a stock runtime makes the game exit at startup with no error.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "C:\Users\renoi\ClaudeCode\Fable 2 Recompile Xbox\rexglue-src"
cmake --build out/build/win-amd64 --config Release --target rexgpu-xenos rexruntime
