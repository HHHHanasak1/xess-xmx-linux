@echo off
rem build.bat - build vktest.exe (XeSS' Vulkan path through vkwrap and the layer, see vktest.cpp) with the Visual
rem Studio 2022 Build Tools. VULKAN_HEADERS: a Vulkan-Headers checkout (its include\ is used). Result: build\vktest.exe
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
if "%VULKAN_HEADERS%"=="" (echo set VULKAN_HEADERS to a Vulkan-Headers checkout & exit /b 1)
if not exist build mkdir build
cl /nologo /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS /I "%VULKAN_HEADERS%\include" vktest.cpp /Fobuild\ /Febuild\vktest.exe || exit /b 1
