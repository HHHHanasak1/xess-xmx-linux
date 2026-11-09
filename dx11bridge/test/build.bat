@echo off
rem build.bat - build the bridge tests with the Visual Studio 2022 Build Tools. Result: build\*.exe
rem   sharetest.exe   a D3D11 and a D3D12 device of one process share a texture and a fence (what the bridge needs)
rem   bridgetest.exe  libxess_dx11.dll next to it: low-res motion vectors + D24S8 depth, output colour, compute state kept
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS sharetest.cpp /Fobuild\ /Febuild\sharetest.exe d3d11.lib d3d12.lib dxgi.lib || exit /b 1
cl /nologo /O2 /EHsc /MT /W3 /D_CRT_SECURE_NO_WARNINGS bridgetest.cpp /Fobuild\ /Febuild\bridgetest.exe d3d11.lib || exit /b 1
