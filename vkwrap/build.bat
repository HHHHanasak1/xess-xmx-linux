@echo off
rem build.bat - build vulkan-1.dll (vkwrap.c + the forwarding stubs gen.py writes) with the Visual Studio 2022 Build
rem Tools. Run "python gen.py exports.txt build" first (it writes the stubs). Result: build\vulkan-1.dll
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
if not exist build mkdir build
if not exist build\stubs.asm (echo run gen.py exports.txt build first & exit /b 1)
ml64 /nologo /c /Fobuild\stubs.obj build\stubs.asm || exit /b 1
cl /nologo /O2 /W3 /MT /LD /D_CRT_SECURE_NO_WARNINGS /I build vkwrap.c build\stubs.obj /Fobuild\ /Fe:build\vulkan-1.dll /link /DEF:build\vulkan-1.def || exit /b 1
