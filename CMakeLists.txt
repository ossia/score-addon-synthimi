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

if(MSVC)
  if(DEFINED ENV{GITHUB_ACTIONS})
    # OOM in kabang
    return()
  endif()
endif()
project(score_addon_synthimi LANGUAGES CXX)

qt_add_resources(QRCS "${CMAKE_CURRENT_SOURCE_DIR}/synthimi.qrc")
avnd_score_plugin_init(
  BASE_TARGET score_addon_synthimi
)

target_link_libraries(score_addon_synthimi
  PRIVATE
    score_plugin_engine score_plugin_avnd
)

set(SYNTHIMI_KFR_INCLUDE_DIR "${OSSIA_3RDPARTY_FOLDER}/kfr/include")

if(TARGET gamma AND TARGET dspfilters AND TARGET r8brain)
  target_link_libraries(score_addon_synthimi
    PRIVATE
      "$<BUILD_INTERFACE:gamma>"
      "$<BUILD_INTERFACE:dspfilters>"
      "$<BUILD_INTERFACE:r8brain>"
  )
else()
  if(EXISTS "${SCORE_SDK}")
    include(FetchContent)
    FetchContent_Declare(synthimi_gamma
      URL https://github.com/LancePutnam/Gamma/archive/65f98a3bfcc476e8434f94ea712c67d0f589321d.tar.gz
      SOURCE_SUBDIR no-cmake
    )
    FetchContent_Declare(synthimi_dspfilters
      URL https://github.com/jcelerier/DSPFilters/archive/b0688e6417a4915b4000ebc41836485911631857.tar.gz
      SOURCE_SUBDIR no-cmake
    )
    FetchContent_Declare(synthimi_r8brain
      URL https://github.com/avaneev/r8brain-free-src/archive/b3f0753d90b3325b8ea8a08fd699e8e6db26e5b5.tar.gz
      SOURCE_SUBDIR no-cmake
    )
    FetchContent_Declare(synthimi_kfr
      URL https://github.com/jcelerier/kfr/archive/32549939f5d4967e5d99327fd431fd5241effc8f.tar.gz
      SOURCE_SUBDIR no-cmake
    )
    FetchContent_MakeAvailable(synthimi_gamma synthimi_dspfilters synthimi_r8brain synthimi_kfr)

    set(SYNTHIMI_GAMMA_DIR "${synthimi_gamma_SOURCE_DIR}")
    set(SYNTHIMI_DSPFILTERS_DIR "${synthimi_dspfilters_SOURCE_DIR}/DSPFilters")
    set(SYNTHIMI_R8BRAIN_DIR "${synthimi_r8brain_SOURCE_DIR}")
    set(SYNTHIMI_KFR_INCLUDE_DIR "${synthimi_kfr_SOURCE_DIR}/include")
  else()
    set(SYNTHIMI_GAMMA_DIR "${3RDPARTY_FOLDER}/Gamma")
    set(SYNTHIMI_DSPFILTERS_DIR "${3RDPARTY_FOLDER}/DSPFilters/DSPFilters")
    set(SYNTHIMI_R8BRAIN_DIR "${OSSIA_3RDPARTY_FOLDER}/r8brain-free-src")
  endif()

  file(GLOB SYNTHIMI_DSPFILTERS_SRCS "${SYNTHIMI_DSPFILTERS_DIR}/source/*.cpp")
  set(SYNTHIMI_DEPENDENCIES_SRCS
    "${SYNTHIMI_GAMMA_DIR}/src/Conversion.cpp"
    "${SYNTHIMI_R8BRAIN_DIR}/r8bbase.cpp"
    ${SYNTHIMI_DSPFILTERS_SRCS}
  )
  set_source_files_properties(${SYNTHIMI_DEPENDENCIES_SRCS}
    PROPERTIES
      SKIP_UNITY_BUILD_INCLUSION ON
      COMPILE_OPTIONS "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>"
  )
  target_sources(score_addon_synthimi PRIVATE ${SYNTHIMI_DEPENDENCIES_SRCS})
  target_include_directories(score_addon_synthimi
    SYSTEM PRIVATE
      "${SYNTHIMI_GAMMA_DIR}"
      "${SYNTHIMI_DSPFILTERS_DIR}/include"
      "${SYNTHIMI_R8BRAIN_DIR}"
  )
endif()

if(OSSIA_ENABLE_KFR AND TARGET kfr)
  target_link_libraries(score_addon_synthimi
    PRIVATE
      "$<BUILD_INTERFACE:kfr>"
  )
else()
  target_include_directories(score_addon_synthimi
    PRIVATE
      "$<BUILD_INTERFACE:${SYNTHIMI_KFR_INCLUDE_DIR}>"
  )
endif()

# Kabang's sampler pitch-shifts through RubberBand; the target PUBLICly
# exports the include dir that satisfies Sampler.hpp's __has_include chain
# (without it the include silently vanishes and RubberBand:: fails to
# resolve on every platform).
if(TARGET rubberband)
  target_link_libraries(score_addon_synthimi
    PRIVATE
      "$<BUILD_INTERFACE:rubberband>"
  )
else()
  target_include_directories(score_addon_synthimi
    PRIVATE
      "$<BUILD_INTERFACE:${OSSIA_3RDPARTY_FOLDER}/rubberband/rubberband>"
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
    Kabang/PlayButton.hpp
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
    Kabang/PlayButton.hpp
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

if(SCORE_TESTING AND TARGET Catch2::Catch2WithMain AND NOT SCORE_FAST_DEV_BUILD)
  add_executable(score_addon_synthimi_mode_test
    tests/SynthimiModeTest.cpp
    Synthimi/SynthimiModel.cpp)
  target_include_directories(score_addon_synthimi_mode_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${OSSIA_3RDPARTY_FOLDER}/kfr/include")
  target_link_libraries(score_addon_synthimi_mode_test PRIVATE
    Catch2::Catch2WithMain score_plugin_avnd gamma dspfilters r8brain)
  add_test(NAME score_addon_synthimi_mode_test COMMAND score_addon_synthimi_mode_test)

  # Kabang / Minibang pitch modulation: pure math, no sampler around it.
  add_executable(score_addon_synthimi_kabang_pitch_envelope_test
    tests/KabangPitchEnvelopeTest.cpp)
  target_include_directories(score_addon_synthimi_kabang_pitch_envelope_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_synthimi_kabang_pitch_envelope_test PRIVATE
    Catch2::Catch2WithMain)
  add_test(NAME score_addon_synthimi_kabang_pitch_envelope_test
    COMMAND score_addon_synthimi_kabang_pitch_envelope_test)

  # A note sent to Minibang with no sample loaded: the drum channel and its
  # sampler, compiled in.
  add_executable(score_addon_synthimi_minibang_no_sample_test
    tests/MinibangNoSampleTest.cpp
    Kabang/Kabang.cpp
    Kabang/Sampler.cpp)
  target_include_directories(score_addon_synthimi_minibang_no_sample_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(score_addon_synthimi_minibang_no_sample_test PRIVATE
    Catch2::Catch2WithMain score_plugin_avnd gamma dspfilters r8brain)
  if(TARGET rubberband)
    target_link_libraries(score_addon_synthimi_minibang_no_sample_test PRIVATE rubberband)
  else()
    target_include_directories(score_addon_synthimi_minibang_no_sample_test PRIVATE
      "${OSSIA_3RDPARTY_FOLDER}/rubberband/rubberband")
  endif()
  add_test(NAME score_addon_synthimi_minibang_no_sample_test
    COMMAND score_addon_synthimi_minibang_no_sample_test)

  add_executable(score_addon_synthimi_fomo_envelope_test tests/FomoEnvelopeTest.cpp)
  target_include_directories(score_addon_synthimi_fomo_envelope_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${OSSIA_3RDPARTY_FOLDER}/kfr/include")
  target_link_libraries(score_addon_synthimi_fomo_envelope_test PRIVATE
    Catch2::Catch2WithMain score_plugin_avnd)
  add_test(NAME score_addon_synthimi_fomo_envelope_test
    COMMAND score_addon_synthimi_fomo_envelope_test)

  # Same compiler requirement as the kaboom object itself
  if(NOT (CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
          AND (CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16
               OR NOT CXX_VERSION_FLAG STREQUAL "cxx_std_26")))
    add_executable(score_addon_synthimi_kaboom_play_test tests/KaboomPlayTest.cpp)
    target_include_directories(score_addon_synthimi_kaboom_play_test PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(score_addon_synthimi_kaboom_play_test PRIVATE
      Catch2::Catch2WithMain score_plugin_avnd)
    add_test(NAME score_addon_synthimi_kaboom_play_test
      COMMAND score_addon_synthimi_kaboom_play_test)
  endif()
endif()

# Documents saved when the envelope times were log knobs, loaded by the
# application with this add-on: the knobs come back as time choosers.
if(SCORE_TESTING AND COMMAND score_add_test)
  score_add_test(score_addon_synthimi_envelope_times_upgrade_test
    SOURCES tests/EnvelopeTimesUpgradeTest.cpp
    APP
    PLUGINS score_plugin_engine score_plugin_scenario score_lib_process)
  add_dependencies(score_addon_synthimi_envelope_times_upgrade_test score_addon_synthimi)
  target_include_directories(score_addon_synthimi_envelope_times_upgrade_test PRIVATE
    # score_test_fixtures carries this include dir, but it is declared after
    # the addons, so its INTERFACE properties are not there to inherit yet.
    "${SCORE_ROOT_SOURCE_DIR}/tests/fixtures")
  target_compile_definitions(score_addon_synthimi_envelope_times_upgrade_test PRIVATE
    SYNTHIMI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
endif()
