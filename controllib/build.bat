@echo off
rem build.bat - build ControlLib.dll (the XESS_FG stand-in for Intel's Control Library) with the Visual Studio 2022 Build Tools.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /W3 /MT /LD ControlLib.c /Fobuild\ /Fe:build\ControlLib.dll /link dxgi.lib dxguid.lib || exit /b 1
