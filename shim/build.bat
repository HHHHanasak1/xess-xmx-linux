@echo off
rem build.bat - build igdext64.dll with the Visual Studio 2022 Build Tools (CMake + Ninja that ship with them).
rem Result: build\igdext64.dll. For a mingw build see toolchain-mingw64.cmake.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set CM=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake
"%CM%\CMake\bin\cmake.exe" -S "%~dp0" -B "%~dp0build" -G Ninja -DCMAKE_MAKE_PROGRAM="%CM%\Ninja\ninja.exe" -DCMAKE_BUILD_TYPE=Release || exit /b 1
"%CM%\CMake\bin\cmake.exe" --build "%~dp0build"
