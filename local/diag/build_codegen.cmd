@echo off
REM Build the CODEGEN TOOL, not the runtime.
REM
REM rexglue.exe is what translates the guest's PowerPC into C++, so a change to
REM an instruction builder only takes effect once this is rebuilt AND the game
REM is regenerated. It is also the one SDK artefact ng2recomp takes from the
REM installed RexBlue tree rather than from the source build, so it has to be
REM copied over as well - deploying the runtime DLLs does not carry it.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=C:\Program Files\LLVM\bin;%PATH%"
cd /d "C:\Users\renoi\ClaudeCode\Fable 2 Recompile Xbox\rexglue-src"
cmake --build out/build/win-amd64 --config Release --target rexglue
