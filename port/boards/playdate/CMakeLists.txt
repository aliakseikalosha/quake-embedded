set(PLAYDATE_GAME_NAME quake)
set(PLAYDATE_GAME_DEVICE quake_DEVICE)

set(PD_SOURCES
	main.c
	keyqueue.c
	autofire.c
	weapons.c
	display.c
	fio.c
	pd_stdio.c
	snd.c
)

if(PD_PROFILE)
	list(APPEND PD_SOURCES pdprof.c)
endif()

if(TOOLCHAIN STREQUAL "armgcc")
	add_executable(${PLAYDATE_GAME_DEVICE} ${PD_SOURCES})
	target_link_libraries(${PLAYDATE_GAME_DEVICE} winquake port)
else()
	add_library(${PLAYDATE_GAME_NAME} SHARED ${PD_SOURCES})
	target_link_libraries(${PLAYDATE_GAME_NAME} winquake port)
endif()

# Put the .pdx in the build tree instead of next to the sources.
set(PRODUCT_DIR ${CMAKE_BINARY_DIR} CACHE STRING "Built PDX directory" FORCE)
include(${SDK}/C_API/buildsupport/playdate_game.cmake)
