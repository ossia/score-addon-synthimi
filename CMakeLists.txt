cmake_minimum_required(VERSION 3.13 FATAL_ERROR)
project(score_addon_synthimi LANGUAGES CXX)

if(NOT TARGET score_lib_base)
  include("${SCORE_SOURCE_DIR}/cmake/ScoreExternalAddon.cmake")
endif()

if(NOT TARGET score_plugin_avnd)
  return()
endif()


if(Qt5Core_VERSION_STRING)
  qt5_add_resources(QRCS "${CMAKE_CURRENT_SOURCE_DIR}/synthimi.qrc")
else()
  qt_add_resources(QRCS "${CMAKE_CURRENT_SOURCE_DIR}/synthimi.qrc")
endif()

add_library(
  score_addon_synthimi
  Synthimi/SynthimiModel.hpp
  Synthimi/SynthimiModel.cpp
  Synthimi/SynthimiUi.hpp
  Synthimi/Synthimi.hpp

  score_addon_synthimi.hpp
  score_addon_synthimi.cpp

  ${QRCS}
)

setup_score_plugin(score_addon_synthimi)

target_link_libraries(score_addon_synthimi
  PRIVATE
    score_plugin_engine score_plugin_avnd
    "$<BUILD_INTERFACE:gamma>"
    "$<BUILD_INTERFACE:dspfilters>"
    "$<BUILD_INTERFACE:r8brain>"
)

if(TARGET kfr)
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
