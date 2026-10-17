# Native Metal GPU path (docs/native-renderer.md). Included at the end of the top-level
# CMakeLists.txt. OFF by default: then this file only declares the option, and the sources in this
# directory (picked up by the runtime's GLOB_RECURSE) compile to empty objects.
#
#   cmake -G Ninja -S . -B build-native -DBOTW_NATIVE_GPU=ON
#   build-native/BotWRecomp --game ../game --renderer native ...
option(BOTW_NATIVE_GPU "Build the native Metal GPU path (run time: --renderer native, --gpu-stats N)" OFF)

if(BOTW_NATIVE_GPU AND TARGET cemu_gpu AND TARGET BotWRecomp)
  # Compile the Cemu glue through latte_switch.mm (renamed to latte_cemu::) instead of on its own,
  # so latte:: can be switched at run time without touching runtime/cemu_shims/latte_glue.mm.
  get_target_property(_gn_cemu_sources cemu_gpu SOURCES)
  list(FILTER _gn_cemu_sources EXCLUDE REGEX "/cemu_shims/latte_glue\\.mm$")
  configure_file(${CMAKE_CURRENT_LIST_DIR}/latte_switch.mm.in ${CMAKE_BINARY_DIR}/gpu_native/latte_switch.mm @ONLY)
  list(APPEND _gn_cemu_sources ${CMAKE_BINARY_DIR}/gpu_native/latte_switch.mm)
  set_property(TARGET cemu_gpu PROPERTY SOURCES ${_gn_cemu_sources})
  target_compile_definitions(cemu_gpu PRIVATE BOTW_NATIVE_GPU=1)
  target_compile_definitions(BotWRecomp PRIVATE BOTW_NATIVE_GPU=1)
  # the shader translator (tools/shaders) doubles as the run-time fallback / phase-1 JIT
  target_sources(BotWRecomp PRIVATE ${CMAKE_SOURCE_DIR}/tools/shaders/src/r600.cpp ${CMAKE_SOURCE_DIR}/tools/shaders/src/msl.cpp)
  message(STATUS "BOTW_NATIVE_GPU: native Metal GPU path enabled (select with --renderer native)")
endif()
