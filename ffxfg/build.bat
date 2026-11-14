@echo off
rem build.bat - build amd_fidelityfx_framegeneration_dx12.dll (FSR frame generation on XeSS-FG; also the optical flow
rem frame generation of OFLOW_FG=1), amd_fidelityfx_upscaler_dx12.dll (FSR upscaling on XeSS SR) and dxgi.dll
rem (FSR_FG=inject, OFLOW_FG=1) with the Visual Studio 2022 Build Tools.
rem Run get-headers.sh first (bash), it puts the AMD and Intel headers into inc\; build-ffxsdk.bat (run from here when
rem needed) builds the FidelityFX SDK 1.1.4 optical flow libraries.
rem FFXFG_DEBUG=1 in the environment builds in the debug switches (FFX_XEFG_DUMP, ORDER, CAMERA, JITTER, RENDERTIME,
rem RENDERSCALE, TRACE, TRACE_SUBS, SLEEP, OWNWAIT, PACE, AB, AB_OLD, HOLD, VALIDITY, MVNDC, HDR10_AB,
rem FFX_XESS_JITTER); the release build leaves them out.
rem The frame generation DLL's source files are ffx_xefg*.cpp (ffx_xefg.h says what each one holds) and, in a debug
rem build, ffx_trace.cpp.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
if not exist inc\ffx\upscalers\include\ffx_upscale.h (echo run get-headers.sh first & exit /b 1)
if not exist ffxsdk\sdk\bin\ffx_sdk\ffx_opticalflow_x64.lib (call build-ffxsdk.bat || exit /b 1)
if not exist build mkdir build
set DEFS=
set FG=ffx_xefg.cpp ffx_xefg_init.cpp ffx_xefg_context.cpp ffx_xefg_swapchain.cpp ffx_xefg_present.cpp ffx_xefg_cap.cpp ffx_xefg_lowlat.cpp ffx_xefg_shown.cpp
if "%FFXFG_DEBUG%"=="1" set DEFS=/DFFXFG_DEBUG
if "%FFXFG_DEBUG%"=="1" set FG=%FG% ffx_trace.cpp
set SDK=ffxsdk\sdk
dxc -nologo -T cs_6_0 -E main -Vn g_oflowExpandCs -Fh build\oflow_expand_cs.h oflow_expand.hlsl || exit /b 1
dxc -nologo -T cs_6_0 -E main -Vn g_oflowDownCs -Fh build\oflow_down_cs.h oflow_down.hlsl || exit /b 1
cl /nologo /c /O2 /W3 /MT /EHsc /std:c++17 /I build /I %SDK%\include ffx_oflow.cpp /Fobuild\ || exit /b 1
cl /nologo /O2 /W3 /MT /LD /EHsc /std:c++17 %DEFS% %FG% build\ffx_oflow.obj /Fobuild\ /Fe:build\amd_fidelityfx_framegeneration_dx12.dll /link dxguid.lib d3d12.lib %SDK%\bin\ffx_sdk\ffx_opticalflow_x64.lib %SDK%\bin\ffx_sdk\ffx_backend_dx12_x64.lib || exit /b 1
cl /nologo /O2 /W3 /MT /LD /EHsc /std:c++17 %DEFS% ffx_xess.cpp /Fobuild\ /Fe:build\amd_fidelityfx_upscaler_dx12.dll || exit /b 1
cl /nologo /O2 /W3 /MT /LD /EHsc /std:c++17 dxgi_xefg.cpp /Fobuild\ /Fe:build\dxgi.dll || exit /b 1
