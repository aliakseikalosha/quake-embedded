# Quake for embedded devices

This project is yet another WinQuake port for embedded devices, primarily for RISC-V devices.

![QuakEMBD on Action](https://i.imgur.com/wctRYIJ.gif)

Based on original [Quake GPL source](https://github.com/id-Software/Quake).

## How to build

Use CMake with [GNU toolchain for RISC-V](https://github.com/riscv-collab/riscv-gnu-toolchain) installed.

Build Instruction:
```shell
git clone https://github.com/sysprog21/quake-embedded && cd quake-embedded
mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../port/boards/rv32emu/toolchain.cmake \
      -DCMAKE_BUILD_TYPE=RELEASE -DBOARD_NAME=rv32emu ..
make
```

## Playdate

Requires the [Playdate SDK](https://play.date/dev/) (`PLAYDATE_SDK_PATH` set) and, for the device, the Arm toolchain the SDK installs.

Game data is not included. Copy your `pak0.pak` (the freely distributable shareware one is fine) to
`port/boards/playdate/Source/id1/pak0.pak` before building, or into the game's Data folder at `id1/`.

```shell
# Simulator
mkdir build-sim && cd build-sim
cmake -DBOARD_NAME=playdate .. && make          # -> quake.pdx

# Device
mkdir build-dev && cd build-dev
cmake -DCMAKE_TOOLCHAIN_FILE=../port/boards/playdate/toolchain.cmake -DBOARD_NAME=playdate .. && make
```

`-DPD_RENDER_WIDTH=400 -DPD_RENDER_HEIGHT=240` renders at full panel resolution (default 320x200, scaled, for speed).

Controls: D-pad move/turn, A fire, B jump, crank switches weapon; the system menu has "Quake Menu", "Always Run" and "Show FPS". In Quake's menus A selects and B goes back.
