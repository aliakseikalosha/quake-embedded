cmake_minimum_required(VERSION 3.8)
project(QuakEMBD C ASM)

if(BOARD_NAME STREQUAL "playdate")
	include(${CMAKE_CURRENT_LIST_DIR}/port/boards/playdate/platform.cmake)
endif()

add_compile_options(-fno-common)
add_definitions(-DWINQUAKE_ENABLE_LOGGING -DWINQUAKE_LOGGING_EXTERNAL)

add_subdirectory(port)
add_subdirectory(winquake)