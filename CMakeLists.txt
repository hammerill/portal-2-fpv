cmake_minimum_required(VERSION 3.24)
project(portal2_fpv CXX)

# Build with: cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Portal 2 is 32-bit: configure with cmake/mingw-w64-i686.cmake")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(LIBFPV_DIR "$ENV{HOME}/Projects/libfpv" CACHE PATH "libfpv source checkout")
set(LIBFPV_BUILD "${LIBFPV_DIR}/build/win32" CACHE PATH "libfpv i686 build dir")
# Local development builds only (tools/deploy.sh), never CI or releases: must
# match a libfpv built with -DLIBFPV_KAMIKAZE=ON (the frame's layout changes).
option(P2FPV_KAMIKAZE "Local dev build against a kamikaze libfpv" OFF)

# portal2-fpv.dll: the server plugin Portal 2 loads (addons/portal2-fpv.vdf).
# It owns the log and the D3D9 hook, and hot-reloads the core.
add_library(p2fpv_loader SHARED
  src/loader.cpp
  src/log.cpp
  src/paths.cpp
)
set_target_properties(p2fpv_loader PROPERTIES PREFIX "" OUTPUT_NAME "portal2-fpv")
target_link_libraries(p2fpv_loader PRIVATE d3d9)

# core.dll: the mod itself.
add_library(p2fpv_core SHARED
  src/core.cpp
  src/crash.cpp
  src/sdk.cpp
  src/fpvapi.cpp
  src/render.cpp
  src/audio.cpp
  src/drone.cpp
  src/world.cpp
  src/gameplay.cpp
  src/pilot.cpp
  src/kamikaze.cpp
  src/devtools.cpp
  src/log.cpp
  src/paths.cpp
)
set_target_properties(p2fpv_core PROPERTIES PREFIX "" OUTPUT_NAME "core")
target_link_libraries(p2fpv_core PRIVATE winmm d3d9 psapi)
target_compile_definitions(p2fpv_core PRIVATE FPVLOG_APPEND)

foreach(target p2fpv_loader p2fpv_core)
  target_include_directories(${target} PRIVATE src "${LIBFPV_DIR}/include")
  # libfpv.dll is loaded at runtime; the header is only used for declarations.
  target_compile_definitions(${target} PRIVATE LIBFPV_STATIC WIN32_LEAN_AND_MEAN NOMINMAX)
  if(P2FPV_KAMIKAZE)
    target_compile_definitions(${target} PRIVATE LIBFPV_KAMIKAZE)
  endif()
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-cast-function-type)
  target_link_options(${target} PRIVATE -static)
endforeach()
