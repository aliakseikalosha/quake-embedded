# Settings shared by every target of a Playdate build (winquake, port, game).
# Included from the top-level CMakeLists.txt, right after project().

set(_pd_sdk "$ENV{PLAYDATE_SDK_PATH}")
if(NOT _pd_sdk)
	execute_process(
		COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config | head -n 1 | cut -c9-"
		OUTPUT_VARIABLE _pd_sdk
		OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
file(TO_CMAKE_PATH "${_pd_sdk}" SDK)
if(NOT EXISTS "${SDK}")
	message(FATAL_ERROR "Playdate SDK not found; set PLAYDATE_SDK_PATH")
endif()

# 3D resolution. Quake's menus assume at least 320x200. The image is drawn
# 1:1 (no scaling), centred on the 400x240 panel; width must be a multiple of 8.
# With PD_LOWRES_3D the 3D view is rendered at half of this size and drawn as 2x2
# pixel patterns (5 grey levels); menus, console and HUD stay at full size.
option(PD_LOWRES_3D "Render the 3D view at half resolution with 2x2 grey patterns" ON)
if(PD_LOWRES_3D)
	set(_pd_default_w 400)
else()
	set(_pd_default_w 320)
endif()
set(PD_RENDER_WIDTH ${_pd_default_w} CACHE STRING "Quake render width (>= 320, multiple of 16 with PD_LOWRES_3D)")
set(PD_RENDER_HEIGHT 240 CACHE STRING "Quake render height (>= 200, even)")

if(NOT CMAKE_BUILD_TYPE)
	set(CMAKE_BUILD_TYPE Release)
endif()

add_compile_definitions(
	QEMBD_PLAYDATE=1
	TARGET_EXTENSION=1
	PD_RENDER_WIDTH=${PD_RENDER_WIDTH}
	PD_RENDER_HEIGHT=${PD_RENDER_HEIGHT}
	$<$<BOOL:${PD_LOWRES_3D}>:PD_LOWRES_3D=1>
	# Quake heap: try DEFAULT, back off in 512 KiB steps down to MIN.
	DEFAULT_MEM_SIZE=\(7*1024*1024\)
	DEFAULT_MIN_MEM_SIZE=\(4*1024*1024\)
)

# stdio -> Playdate file API, printf -> console log
add_compile_options("$<$<COMPILE_LANGUAGE:C>:SHELL:-include ${CMAKE_CURRENT_LIST_DIR}/pd_compat.h>")

if(TOOLCHAIN STREQUAL "armgcc")
	set(_pd_mcflags -mthumb -mcpu=cortex-m7 -mfloat-abi=hard -mfpu=fpv5-sp-d16 -D__FPU_USED=1)
	add_compile_definitions(TARGET_PLAYDATE=1)
	add_compile_options(${_pd_mcflags}
		-falign-functions=16 -fomit-frame-pointer
		-ffunction-sections -fdata-sections -mword-relocations)
else()
	set(CMAKE_POSITION_INDEPENDENT_CODE ON)
	add_compile_definitions(TARGET_SIMULATOR=1)
endif()
