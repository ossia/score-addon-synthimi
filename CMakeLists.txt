cmake_minimum_required(VERSION 3.24 FATAL_ERROR)

project(score_addon_synthimi LANGUAGES CXX)

if("${CMAKE_SYSTEM_PROCESSOR}" MATCHES armhf)
  return()
endif()

# Use the Avendish the host (ossia score) provides; otherwise fetch it.
find_package(Avendish QUIET)
if(NOT Avendish_FOUND)
  include(FetchContent)
  FetchContent_Declare(
    Avendish
    GIT_REPOSITORY "https://github.com/celtera/avendish"
    GIT_TAG main)
  FetchContent_Populate(Avendish)
  list(APPEND CMAKE_PREFIX_PATH "${avendish_SOURCE_DIR}")
  find_package(Avendish REQUIRED)
endif()

qt_add_resources(QRCS "${CMAKE_CURRENT_SOURCE_DIR}/synthimi.qrc")

# Audio dependencies, linked so they reach the score base target / every back-end.
add_library(synthimi_deps INTERFACE)
target_link_libraries(synthimi_deps
  INTERFACE
    "$<BUILD_INTERFACE:gamma>"
    "$<BUILD_INTERFACE:dspfilters>"
    "$<BUILD_INTERFACE:r8brain>")
if(OSSIA_ENABLE_KFR AND TARGET kfr)
  target_link_libraries(synthimi_deps INTERFACE "$<BUILD_INTERFACE:kfr>")
else()
  target_include_directories(synthimi_deps
    INTERFACE "$<BUILD_INTERFACE:${OSSIA_3RDPARTY_FOLDER}/kfr/include>")
endif()

avnd_addon_init(NAME score_addon_synthimi)

avnd_addon_object(
  BASE score_addon_synthimi C_NAME synthimi CLASS Synthimi NAMESPACE Synthimi
  DEPENDENCIES synthimi_deps
  SOURCES
    Synthimi/Synthimi.hpp
    Synthimi/SynthimiModel.hpp
    Synthimi/SynthimiModel.cpp
    Synthimi/SynthimiUi.hpp
    ${QRCS})

avnd_addon_finalize(NAME score_addon_synthimi UUID 59d3b321-2f74-4c13-ad1c-72af76c6bb1d VERSION 1)
