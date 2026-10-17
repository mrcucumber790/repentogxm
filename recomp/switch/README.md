# Nintendo Switch host

The Switch build shares the 64-bit host layer in `recomp/linux64` (guest
memory window, POSIX implementations of the Vita calls, SDL2 renderer, Lua,
OpenAL). This directory holds only what differs on libnx:

- `main_switch.c` is the entry point. It runs the game on an 8 MiB thread.
- `switch_paths.c` registers `ux0:` and `app0:` as devices over
  `sdmc:/switch/isaac`.
- `switch_malloc.c` routes newlib's allocator to the guest heap.

Guest memory is the libnx heap (the arena layout in `guest_window.c`): the
PE image sits at `GUEST_IMAGE_BASE` at the start of the heap, and the guest
heap follows it, up to about 1.4 GiB.

## Build

You need devkitPro with `switch-dev`, `switch-sdl2` and `switch-openal-soft`.

```
cmake -G Ninja -S recomp/linux64 -B build/switch \
  -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DISAAC_GENERATED_DIR=/abs/path/to/generated \
  -DISAAC_LUA53_SOURCE_DIR=/abs/path/to/lua-5.3.3
ninja -C build/switch
```

The build produces `build/switch/isaac.nro`.

## SD card layout

```
sdmc:/switch/isaac/isaac.nro
sdmc:/switch/isaac/isaac-ng.exe.unpacked.exe   your unpacked 1.7.9b J835 exe
sdmc:/switch/isaac/resources/                  resources/ from your PC install
sdmc:/switch/isaac/mods/                       Lua mods (one folder each)
```

Saves and the game's `log.txt` go under `sdmc:/switch/isaac/Documents/`. The
port's own log is `sdmc:/switch/isaac/first-arm-fault.log`.

Launch the game with full memory: hold R while starting any installed game
(title override). Launching from the album gives too little memory.
