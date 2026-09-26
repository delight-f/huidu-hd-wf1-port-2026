cmake_minimum_required(VERSION 3.16.0)

# Generate secrets.cmake from secrets.json if it exists
execute_process(
    COMMAND ${PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/generate_secrets_cmake.py
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE _generate_secrets_result
)
if(NOT _generate_secrets_result EQUAL 0)
    message(FATAL_ERROR "generate_secrets_cmake.py failed (exit ${_generate_secrets_result}); see output above")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware)

# Apply or revert the ESP32-HUB75-MatrixPanel-DMA I2S divider patch based on
# CONFIG_PATCH_I2S_DIVIDER, so switching between patched/unpatched configs
# without a full clean still leaves managed_components/ in the right state.
if(CONFIG_PATCH_I2S_DIVIDER)
    set(_patch_i2s_mode "--apply")
else()
    set(_patch_i2s_mode "--revert")
endif()
execute_process(
    COMMAND ${PYTHON} ${CMAKE_CURRENT_SOURCE_DIR}/tools/patch_i2s_divider.py ${_patch_i2s_mode}
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    RESULT_VARIABLE _patch_i2s_result
)
if(NOT _patch_i2s_result EQUAL 0)
    message(WARNING "patch_i2s_divider.py ${_patch_i2s_mode} exited non-zero (${_patch_i2s_result}); see output above")
endif()

# Keep the HUB75 driver's compile-time colour depth in step with the run-time one.
#
# The driver picks its CIE correction table from PIXEL_COLOR_DEPTH_BITS *and* the
# bits it reads back out of it, so the two must agree or every channel becomes a
# sawtooth and colours invert - which is what happened when the panel depth was
# trimmed for memory (see HANDOFF trap 11). The library defaults that macro to 8.
# The WF1 runs display.cpp's kPanelColorDepthBits at 6 to stay inside its RAM, and
# 6 is one of the depths cie_luts.h carries a native table for. Set from here
# rather than inside managed_components/, which is refetched.
if(CONFIG_BOARD_HUIDU_WF1)
    idf_component_get_property(_hub75_lib esp32-hub75-matrixpanel-dma COMPONENT_LIB)
    if(_hub75_lib)
        target_compile_definitions(${_hub75_lib} PRIVATE PIXEL_COLOR_DEPTH_BITS=6)
        message(STATUS "HD-WF1: building the HUB75 driver at PIXEL_COLOR_DEPTH_BITS=6")
    else()
        message(WARNING "HD-WF1: could not find the HUB75 component to set its colour depth")
    endif()
endif()
