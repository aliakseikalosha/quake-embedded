add_library(port OBJECT
	in_port.c
	cd_null.c
	sys_port.c
	vid_port.c
)

# Playdate has its own sound backend (boards/playdate/snd.c)
if(NOT BOARD_NAME STREQUAL "playdate")
	target_sources(port PRIVATE snd.c)
endif()

target_include_directories(port PUBLIC
	${PROJECT_SOURCE_DIR}/include
	${PROJECT_SOURCE_DIR}/winquake
)

add_subdirectory(boards/${BOARD_NAME})
