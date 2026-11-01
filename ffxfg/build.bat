@echo off
rem build.bat - build amd_fidelityfx_framegeneration_dx12.dll (FSR frame generation on XeSS-FG) with the Visual Studio 2022
rem Build Tools. Run get-headers.sh first (bash), it puts the AMD and Intel headers into inc\.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist inc\ffx\api\include\ffx_api.h (echo run get-headers.sh first & exit /b 1)
if not exist build mkdir build
cl /nologo /O2 /W3 /MT /LD /EHsc /std:c++17 ffx_xefg.cpp /Fobuild\ /Fe:build\amd_fidelityfx_framegeneration_dx12.dll /link dxguid.lib || exit /b 1
