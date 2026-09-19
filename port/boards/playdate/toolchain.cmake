# Toolchain file for the Playdate *device* build:
#   cmake -DCMAKE_TOOLCHAIN_FILE=../port/boards/playdate/toolchain.cmake \
#         -DBOARD_NAME=playdate ..
# Omit the toolchain file to build the Simulator library on the host instead.

set(_pd_sdk "$ENV{PLAYDATE_SDK_PATH}")
if(NOT _pd_sdk)
	execute_process(
		COMMAND bash -c "egrep '^\\s*SDKRoot' $HOME/.Playdate/config | head -n 1 | cut -c9-"
		OUTPUT_VARIABLE _pd_sdk
		OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()
if(NOT EXISTS "${_pd_sdk}/C_API/buildsupport/arm.cmake")
	message(FATAL_ERROR "Playdate SDK not found; set PLAYDATE_SDK_PATH")
endif()

include(${_pd_sdk}/C_API/buildsupport/arm.cmake)
