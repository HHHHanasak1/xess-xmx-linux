@echo off
rem build-ffxsdk.bat - the FidelityFX SDK 1.1.4 static libraries the optical flow (OFLOW_FG=1, ffx_oflow.cpp) links:
rem ffx_opticalflow_x64.lib and ffx_backend_dx12_x64.lib, in ffxsdk\sdk\bin\ffx_sdk\ (not kept in git).
rem GPUOpen-LibrariesAndSDKs/FidelityFX-SDK, tag v1.1.4 (MIT), checked against its commit. Needs git and cmake (the
rem Visual Studio 2022 Build Tools bring cmake). build.bat runs it when the libraries are missing.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%~dp0"
set COMMIT=c6efa6bf7f2027b3ec94f28578bb5965eabb9e55
if not exist ffxsdk\sdk (
  git clone --quiet --depth 1 --branch v1.1.4 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git ffxsdk || exit /b 1
)
for /f %%c in ('git -C ffxsdk rev-parse HEAD') do set HEAD=%%c
if not "%HEAD%"=="%COMMIT%" (echo ffxsdk is at %HEAD%, expected %COMMIT% & exit /b 1)
rem no -G: cmake takes the newest Visual Studio there is (GitHub's windows-latest has no 2022 any more)
cmake -S ffxsdk\sdk -B ffxsdk\sdk\build-of -A x64 -DFFX_API_BACKEND=DX12_X64 -DFFX_ALL=OFF ^
  -DFFX_FSR3=ON -DFFX_FSR3UPSCALER=OFF -DFFX_FI=OFF -DFFX_OF=ON -DFFX_BUILD_AS_DLL=OFF -DCMAKE_POLICY_VERSION_MINIMUM=3.5 ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded || exit /b 1
cmake --build ffxsdk\sdk\build-of --config Release -- /m /v:minimal || exit /b 1
if not exist ffxsdk\sdk\bin\ffx_sdk\ffx_opticalflow_x64.lib (echo ffx_opticalflow_x64.lib missing & exit /b 1)
if not exist ffxsdk\sdk\bin\ffx_sdk\ffx_backend_dx12_x64.lib (echo ffx_backend_dx12_x64.lib missing & exit /b 1)
