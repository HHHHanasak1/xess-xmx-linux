@echo off
rem build.bat - build libxess_dx11.dll (XeSS for D3D11 games on the D3D12 XeSS, see xess_dx11.cpp) with the Visual
rem Studio 2022 Build Tools. Run ..\ffxfg\get-headers.sh first (bash): the XeSS headers come from there.
rem Result: build\libxess_dx11.dll
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
if not exist ..\ffxfg\inc\xess\xess\xess_d3d11.h (echo run ..\ffxfg\get-headers.sh first & exit /b 1)
if not exist build mkdir build
fxc /nologo /T cs_5_0 /E main /O3 /Vn g_depthCopyCs /Fh build\depth_copy_cs.h depth_copy.hlsl || exit /b 1
rc /nologo /fo build\version.res version.rc || exit /b 1
cl /nologo /O2 /W3 /MT /LD /EHsc /std:c++17 /D_CRT_SECURE_NO_WARNINGS /I build xess_dx11.cpp build\version.res /Fobuild\ /Fe:build\libxess_dx11.dll /link /DEF:libxess_dx11.def d3d11.lib d3d12.lib dxgi.lib || exit /b 1
