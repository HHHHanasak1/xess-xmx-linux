@echo off
rem build.bat - build amd_fidelityfx_framegeneration_dx12.dll (FSR frame generation on XeSS-FG) and
rem amd_fidelityfx_upscaler_dx12.dll (FSR upscaling on XeSS SR) and dxgi.dll (FSR_FG=inject) with the Visual Studio 2022 Build Tools.
rem Run get-headers.sh first (bash), it puts the AMD and Intel headers into inc\.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist inc\ffx\upscalers\include\ffx_upscale.h (echo run get-headers.sh first & exit /b 1)
if not exist build mkdir build
cl /nologo /O2 /W3 /MT /LD /EHsc /std:c++17 ffx_xefg.cpp /Fobuild\ /Fe:build\amd_fidelityfx_framegeneration_dx12.dll /link dxguid.lib || exit /b 1
cl /nologo /O2 /W3 /MT /LD /EHsc /std:c++17 ffx_xess.cpp /Fobuild\ /Fe:build\amd_fidelityfx_upscaler_dx12.dll || exit /b 1
cl /nologo /O2 /W3 /MT /LD /EHsc /std:c++17 dxgi_xefg.cpp /Fobuild\ /Fe:build\dxgi.dll || exit /b 1
