cmake_minimum_required(VERSION 3.15)
# Static MSVC runtime: the shim is loaded into whatever CRT the game prefix has (Resident Evil 4's prefix carries an
# older msvcp140 in which the dynamic build crashed inside DllMain and XeSS silently fell back to DP4a).
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(dxvk-igdext LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SRC_DIR         "${CMAKE_CURRENT_SOURCE_DIR}/src")
set(LEGACY_DIR      "${SRC_DIR}/legacy")
set(DLL_DIR         "${SRC_DIR}/dll")

# Vendored Intel Graphics Extensions public SDK header (igdext.h)
set(D3DEXT_SDK_PUBLIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/d3dext-sdk/include/public")

set(SOURCES
    ${SRC_DIR}/ExtensionContextBase.cpp
    ${SRC_DIR}/D3D11ExtensionContext.cpp
    ${SRC_DIR}/D3D12ExtensionContext.cpp
    ${LEGACY_DIR}/LegacyD3D11ExtensionContext.cpp
    ${LEGACY_DIR}/LegacyD3D12ExtensionContext.cpp
    ${DLL_DIR}/IgdextApiDll.cpp
    ${DLL_DIR}/D3D12Api.cpp
    ${DLL_DIR}/XellHook.cpp
    ${DLL_DIR}/GpuInfo.cpp
    ${DLL_DIR}/XellDriver.cpp
    ${SRC_DIR}/Trace.cpp
    ${DLL_DIR}/IgdextApiDllInt.cpp
    ${DLL_DIR}/IgdextApiDllLegacy.cpp
    ${DLL_DIR}/Igdext.def
)

add_library(igdext SHARED ${SOURCES})

target_include_directories(igdext PRIVATE
    ${SRC_DIR}
    ${LEGACY_DIR}
    ${D3DEXT_SDK_PUBLIC_DIR}
)

target_compile_definitions(igdext PRIVATE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    # Both halves of the vendored igdext.h. The D3D12 section (behind
    # INTC_IGDEXT_D3D12) has the types of the _INTC_D3D12_* functions in
    # src/dll/D3D12Api.cpp; it references D3D12_STATE_OBJECT_DESC and other
    # raytracing types, which an old mingw-w64 d3d12.h does not define.
    INTC_IGDEXT_D3D11
    INTC_IGDEXT_D3D12
)

target_link_libraries(igdext PRIVATE dxgi)

set_target_properties(igdext PROPERTIES
    OUTPUT_NAME "igdext64"
    PREFIX ""
)

if( MINGW )
    target_link_options(igdext PRIVATE -static -static-libgcc -static-libstdc++)
endif()

include(GNUInstallDirs)
install(TARGETS igdext RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
