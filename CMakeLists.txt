cmake_minimum_required(VERSION 3.24)
project(BotWRecomp LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.3)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# Recompiled guest code (tools/recompile.py writes generated/).
file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/generated/fn_*.cpp
  ${CMAKE_SOURCE_DIR}/generated/big_*.cpp
  ${CMAKE_SOURCE_DIR}/generated/huge_*.cpp
  ${CMAKE_SOURCE_DIR}/generated/table.cpp
  ${CMAKE_SOURCE_DIR}/generated/hle_stubs.cpp)
add_library(guest OBJECT ${GUEST_SOURCES})
target_include_directories(guest PRIVATE runtime/include generated)
target_compile_options(guest PRIVATE -O2 -gline-tables-only -w -fwrapv -fno-strict-aliasing -mcpu=apple-m1
  -ffp-contract=off)
file(GLOB BIG_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/big_*.cpp)
file(GLOB HUGE_SOURCES CONFIGURE_DEPENDS ${CMAKE_SOURCE_DIR}/generated/huge_*.cpp)
set_source_files_properties(${BIG_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")
set_source_files_properties(${HUGE_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0")

# Vendored Cemu Latte GPU layer (runtime/cemu, MPL-2.0) + shims, built in Cemu's own environment.
file(GLOB_RECURSE CEMU_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Common/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu/util/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cemu/FileCache/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/imgui/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.mm)
list(FILTER CEMU_SOURCES EXCLUDE REGEX "LatteOverlay\\.cpp$|/dependencies/|SystemInfo(Win|Linux|BSD)\\.cpp$")
list(APPEND CEMU_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_draw.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_tables.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_widgets.cpp)
add_library(cemu_gpu OBJECT ${CEMU_SOURCES})
target_include_directories(cemu_gpu PRIVATE runtime/cemu_shims runtime/cemu runtime/cemu/Cafe
  runtime/cemu/dependencies/metal-cpp runtime/cemu/dependencies/imgui /opt/homebrew/include)
target_compile_definitions(cemu_gpu PRIVATE BOOST_NOWIDE_NO_LIB ENABLE_METAL BOTWRECOMP IMGUI_IMPL_METAL_CPP
  EMULATOR_VERSION_MAJOR=2 EMULATOR_VERSION_MINOR=6 EMULATOR_VERSION_PATCH=0 EMULATOR_HASH=a53cf0e)
target_compile_options(cemu_gpu PRIVATE -w -O2 -g1 "SHELL:-include Common/precompiled.h")

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/src/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/*.mm)
add_executable(BotWRecomp ${RUNTIME_SOURCES} $<TARGET_OBJECTS:guest> $<TARGET_OBJECTS:cemu_gpu>)
target_include_directories(BotWRecomp PRIVATE runtime/include runtime/src generated)
target_compile_options(BotWRecomp PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -fwrapv -mcpu=apple-m1
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
find_package(SDL3 REQUIRED CONFIG)
find_package(fmt REQUIRED)
target_link_libraries(BotWRecomp PRIVATE z SDL3::SDL3 fmt::fmt
  /opt/homebrew/opt/openssl@3/lib/libcrypto.dylib /opt/homebrew/opt/zstd/lib/libzstd.dylib
  "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit" "-framework MetalFX"
  "-framework CoreGraphics" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")

# Native Metal GPU path (docs/native-renderer.md): option BOTW_NATIVE_GPU, default OFF (gpu_native.cmake).
include(${CMAKE_SOURCE_DIR}/runtime/src/gpu_native/gpu_native.cmake OPTIONAL)
