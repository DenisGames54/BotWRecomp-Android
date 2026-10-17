# Offline shader pipeline for the native renderer (docs/native-renderer.md).
#   cmake -G Ninja -S tools/shaders -B build-native/shaders-tool && ninja -C build-native/shaders-tool
#   build-native/shaders-tool/latte2msl --cemu runtime/cemu --out build-native/shaders <cache.bin>...
#   uv run python -I tools/shaders/compile_all.py build-native/shaders
cmake_minimum_required(VERSION 3.24)
project(latte_shaders CXX OBJCXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()
add_executable(latte2msl src/main.cpp src/inventory.cpp src/r600.cpp src/msl.cpp)
target_include_directories(latte2msl PRIVATE /opt/homebrew/include)
target_compile_options(latte2msl PRIVATE -Wall -Wno-unused-function)
target_link_libraries(latte2msl PRIVATE /opt/homebrew/opt/zstd/lib/libzstd.dylib z)

# Metal-framework compile check (no offline toolchain needed)
add_executable(mtlcheck src/mtlcheck.mm)
target_compile_options(mtlcheck PRIVATE -fobjc-arc)
target_link_libraries(mtlcheck PRIVATE "-framework Metal" "-framework Foundation")
