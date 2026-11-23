# DXVK-native's Direct3D 11 and DXGI (the renderer's Direct3D off Windows):
# the target gt3_dxvk, its headers (d3d11.h, dxgi*.h, windows_base.h) and
# libdxvk_d3d11 / libdxvk_dxgi.  A system package by pkg-config (dxvk-d3d11,
# dxvk-dxgi), else the i686 build in GT3PC_DXVK_ROOT (~/opt/i386: its .pc
# files, else its headers and libraries by path).  GT3PC_DXVK_LIBDIR: the
# libraries' folder.  The libraries find each other and SDL3 at run time
# (DXVK_WSI_DRIVER=SDL3, set by platform/gpu.cpp): with ~/opt/i386 the
# program runs with LD_LIBRARY_PATH=$HOME/opt/i386/lib.  Not found: an error,
# or with GT3PC_DXVK_OPTIONAL no target (the tests).
if(TARGET gt3_dxvk)
    return()
endif()

set(GT3PC_DXVK_ROOT "$ENV{HOME}/opt/i386" CACHE PATH "DXVK-native's prefix when it is not a system package")

find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(GT3PC_DXVK QUIET IMPORTED_TARGET GLOBAL dxvk-d3d11 dxvk-dxgi)
    if(GT3PC_DXVK_FOUND)
        add_library(gt3_dxvk INTERFACE)
        target_link_libraries(gt3_dxvk INTERFACE PkgConfig::GT3PC_DXVK)
        set(GT3PC_DXVK_LIBDIR ${GT3PC_DXVK_LIBRARY_DIRS})
        message(STATUS "DXVK-native ${GT3PC_DXVK_dxvk-d3d11_VERSION} (system, pkg-config)")
        return()
    endif()
    if(EXISTS "${GT3PC_DXVK_ROOT}/lib/pkgconfig/dxvk-d3d11.pc")
        set(_gt3pc_pkg_path "$ENV{PKG_CONFIG_PATH}")
        set(ENV{PKG_CONFIG_PATH} "${GT3PC_DXVK_ROOT}/lib/pkgconfig:${_gt3pc_pkg_path}")
        pkg_check_modules(GT3PC_DXVK_PREFIXED QUIET IMPORTED_TARGET GLOBAL dxvk-d3d11 dxvk-dxgi)
        set(ENV{PKG_CONFIG_PATH} "${_gt3pc_pkg_path}")
        if(GT3PC_DXVK_PREFIXED_FOUND)
            add_library(gt3_dxvk INTERFACE)
            target_link_libraries(gt3_dxvk INTERFACE PkgConfig::GT3PC_DXVK_PREFIXED)
            set(GT3PC_DXVK_LIBDIR ${GT3PC_DXVK_PREFIXED_LIBRARY_DIRS})
            message(STATUS "DXVK-native ${GT3PC_DXVK_PREFIXED_dxvk-d3d11_VERSION} (${GT3PC_DXVK_ROOT}, pkg-config)")
            return()
        endif()
    endif()
endif()

find_path(GT3PC_DXVK_INCLUDE_DIR d3d11.h PATHS "${GT3PC_DXVK_ROOT}/include/dxvk" NO_DEFAULT_PATH)
find_library(GT3PC_DXVK_D3D11 dxvk_d3d11 PATHS "${GT3PC_DXVK_ROOT}/lib" NO_DEFAULT_PATH)
find_library(GT3PC_DXVK_DXGI dxvk_dxgi PATHS "${GT3PC_DXVK_ROOT}/lib" NO_DEFAULT_PATH)
if(NOT GT3PC_DXVK_INCLUDE_DIR OR NOT GT3PC_DXVK_D3D11 OR NOT GT3PC_DXVK_DXGI)
    if(GT3PC_DXVK_OPTIONAL)
        message(STATUS "DXVK-native not found (pkg-config dxvk-d3d11 / dxvk-dxgi, ${GT3PC_DXVK_ROOT})")
        return()
    endif()
    message(FATAL_ERROR "DXVK-native not found: no pkg-config dxvk-d3d11 / dxvk-dxgi, nor "
                        "${GT3PC_DXVK_ROOT}/include/dxvk and its libdxvk_d3d11 / libdxvk_dxgi "
                        "(set GT3PC_DXVK_ROOT)")
endif()
add_library(gt3_dxvk INTERFACE)
target_include_directories(gt3_dxvk INTERFACE ${GT3PC_DXVK_INCLUDE_DIR})
target_link_libraries(gt3_dxvk INTERFACE ${GT3PC_DXVK_D3D11} ${GT3PC_DXVK_DXGI})
get_filename_component(GT3PC_DXVK_LIBDIR ${GT3PC_DXVK_D3D11} DIRECTORY)
message(STATUS "DXVK-native (${GT3PC_DXVK_ROOT}, by path)")
