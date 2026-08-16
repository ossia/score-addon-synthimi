cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

if("${CMAKE_SYSTEM_PROCESSOR}" MATCHES armhf)
  return()
endif()

if(NOT TARGET score_lib_base)
  include(ScoreExternalAddon)
endif()

if(NOT TARGET score_plugin_avnd)
  return()
endif()

project(score_addon_synthimi LANGUAGES CXX)

qt_add_resources(QRCS "${CMAKE_CURRENT_SOURCE_DIR}/synthimi.qrc")
avnd_score_plugin_init(
  BASE_TARGET score_addon_synthimi
)

target_link_libraries(score_addon_synthimi
  PRIVATE
    score_plugin_engine score_plugin_avnd
    "$<BUILD_INTERFACE:gamma>"
    "$<BUILD_INTERFACE:dspfilters>"
    "$<BUILD_INTERFACE:r8brain>"
)

if(OSSIA_ENABLE_KFR AND TARGET kfr)
  target_link_libraries(score_addon_synthimi
    PRIVATE
      "$<BUILD_INTERFACE:kfr>"
  )
else()
  target_include_directories(score_addon_synthimi
    PRIVATE
      "$<BUILD_INTERFACE:${OSSIA_3RDPARTY_FOLDER}/kfr/include>"
  )
endif()

avnd_score_plugin_add(
  BASE_TARGET score_addon_synthimi
  SOURCES
    Synthimi/Synthimi.hpp
    Synthimi/SynthimiModel.hpp
    Synthimi/SynthimiModel.cpp
    Synthimi/SynthimiUi.hpp
    ${QRCS}
  TARGET synthimi
  MAIN_CLASS Synthimi
  NAMESPACE Synthimi
  OPTIMIZED
)

avnd_score_plugin_add(
  BASE_TARGET score_addon_synthimi
  SOURCES
    Fomo/Fomo.hpp
  TARGET fomo
  MAIN_CLASS Fomo
  NAMESPACE Synthimi
  OPTIMIZED
)

# Kaboom's port count is large enough that instantiating it through the
# boost.pfr fallback exhausts the compiler: g++-14 peaks at 40 GB and is
# OOM-killed, and -fsyntax-only alone reaches 10 GB, so no optimisation flag
# helps. GCC only leaves that fallback with P1061 packs: g++ >= 16, as C++26.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16
        OR NOT CXX_VERSION_FLAG STREQUAL "cxx_std_26"))
  message(STATUS "score-addon-synthimi: skipping kaboom, it needs g++ >= 16 in C++26 mode")
else()
avnd_score_plugin_add(
  BASE_TARGET score_addon_synthimi
  SOURCES
    Kaboom/Kaboom.hpp
  TARGET kaboom
  MAIN_CLASS Kaboom
  NAMESPACE kbm
  OPTIMIZED
)
endif()

# At the default inline threshold clang spends ~3h30 in the WebAssembly backend
# lowering this one object; 40 is the highest value that still compiles it in
# under two minutes. Set on the generated TU alone so no other object changes,
# and -fno-lto so the inliner actually runs here rather than at link time.
if("${CMAKE_CXX_COMPILER_ID}" MATCHES ".*Clang")
  set_property(SOURCE "${CMAKE_BINARY_DIR}/kaboom_avnd.cpp" APPEND PROPERTY
    COMPILE_OPTIONS -fno-lto -mllvm -inline-threshold=40)
endif()

# Kabang and Minibang, moved here from avendish's examples: they are score
# objects, not avendish demos, and building them from the examples tree meant
# score-plugin-avnd carried sources it did not own.
#
# The guard is inherited from that move: the objects use P1061 structured-binding
# packs, which g++ only supports from 16 in C++26 mode, and they are not built
# for the web.
if(NOT EMSCRIPTEN AND NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
                           AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16
                                OR NOT CXX_VERSION_FLAG STREQUAL "cxx_std_26")))
avnd_score_plugin_add(
  BASE_TARGET score_addon_synthimi
  SOURCES
    Kabang/Kabang.hpp
    Kabang/Kabang.cpp
    Kabang/Sampler.hpp
    Kabang/Sampler.cpp
  TARGET kabang
  MAIN_CLASS Kabang
  NAMESPACE kbng
  OPTIMIZED
)

avnd_score_plugin_add(
  BASE_TARGET score_addon_synthimi
  SOURCES
    Kabang/Minibang.hpp
    Kabang/Kabang.cpp
    Kabang/Sampler.hpp
    Kabang/Sampler.cpp
  TARGET minibang
  MAIN_CLASS Minibang
  NAMESPACE kbng
  OPTIMIZED
)

# Kabang's port count puts it in the range where clang's inliner turns the
# generated walkers into functions the backend takes hours to lower. 40 is the
# highest threshold that still compiles quickly. Only the generated TU is
# touched, and -fno-lto keeps the inliner here instead of at link time.
# Minibang is the reduced variant -- 4 ports against 45 -- and does not need it.
if("${CMAKE_CXX_COMPILER_ID}" MATCHES ".*Clang")
  set_property(SOURCE "${CMAKE_BINARY_DIR}/kabang_avnd.cpp" APPEND PROPERTY
    COMPILE_OPTIONS -fno-lto -mllvm -inline-threshold=40)
endif()
endif()

avnd_score_plugin_finalize(
  BASE_TARGET score_addon_synthimi
  PLUGIN_VERSION 1
  PLUGIN_UUID "59d3b321-2f74-4c13-ad1c-72af76c6bb1d"
)
