# 64-bit Linux host for the recompiled game (step one of the Switch port).
#
#   cmake -S recomp/linux64 -B build/linux64 \
#         -DISAAC_GENERATED_DIR=/abs/path/to/generated
#   cmake --build build/linux64 -j
#
# The shared runtime (recomp/runtime) is compiled with its Vita code paths;
# compat/ supplies the few VitaSDK headers it includes and psp2_posix.c
# implements those calls over POSIX.  Guest memory lives in a 4 GiB window
# (GUEST_MEM_WINDOW=1, guest_window.c).
cmake_minimum_required(VERSION 3.16)
project(isaac_linux64 C)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "build type" FORCE)
endif()

set(ISAAC_GENERATED_DIR "" CACHE PATH "gen_all.py output directory")
set(ISAAC_GUEST_BASE "0x98000000" CACHE STRING "guest image base")
if(NOT IS_DIRECTORY "${ISAAC_GENERATED_DIR}" OR
   NOT EXISTS "${ISAAC_GENERATED_DIR}/guest_funcs.h")
  message(FATAL_ERROR "ISAAC_GENERATED_DIR must name fresh gen_all.py output")
endif()
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "this host is for 64-bit targets")
endif()

set(ISAAC_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/../runtime")
set(ISAAC_VITA "${CMAKE_CURRENT_SOURCE_DIR}/../vita")

file(GLOB ISAAC_GENERATED_C "${ISAAC_GENERATED_DIR}/guest_*.c")

set(ISAAC_RUNTIME_SOURCES
  "${ISAAC_RUNTIME}/guest.c"
  "${ISAAC_RUNTIME}/entry_vita.c"
  "${ISAAC_RUNTIME}/host_vita_first_fault.c"
  "${ISAAC_RUNTIME}/host_vita_import_id.c"
  "${ISAAC_RUNTIME}/host_vita_com.c"
  "${ISAAC_RUNTIME}/host_vita_console.c"
  "${ISAAC_RUNTIME}/host_vita_crt.c"
  "${ISAAC_RUNTIME}/host_vita_exception.c"
  "${ISAAC_RUNTIME}/host_vita_file_lock.c"
  "${ISAAC_RUNTIME}/host_vita_filesystem.c"
  "${ISAAC_RUNTIME}/host_vita_find.c"
  "${ISAAC_RUNTIME}/host_vita_fls.c"
  "${ISAAC_RUNTIME}/host_vita_gl.c"
  "${ISAAC_RUNTIME}/host_vita_heap.c"
  "${ISAAC_RUNTIME}/kage_vita_fx_rollback.c"
  "${ISAAC_RUNTIME}/host_vita_math.c"
  "${ISAAC_RUNTIME}/host_vita_sync.c"
  "${ISAAC_RUNTIME}/host_vita_memory.c"
  "${ISAAC_RUNTIME}/host_vita_post_com.c"
  "${ISAAC_RUNTIME}/host_vita_rtti.c"
  "${ISAAC_RUNTIME}/host_vita_startup.c"
  "${ISAAC_RUNTIME}/host_vita_steam.c"
  "${ISAAC_RUNTIME}/host_vita_user32.c"
  "${ISAAC_RUNTIME}/vita_sync_services.c"
  "${ISAAC_RUNTIME}/gl_bridge.c"
  "${ISAAC_RUNTIME}/manual_portable.c")

# Renderer: SDL2 window + desktop GL through the PC KAGE boundary
# (manual_kage.c, gl_pc_backend.c), or inert hooks for the first-fault stage.
option(ISAAC_SDL "SDL2/OpenGL renderer" ON)
if(ISAAC_SDL)
  find_package(SDL2 REQUIRED)
  list(APPEND ISAAC_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/manual_kage.c"
    "${ISAAC_RUNTIME}/gl_pc_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdl/kage_sdl_backend.c")
else()
  list(APPEND ISAAC_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/kage_pc_disabled.c"
    "${ISAAC_RUNTIME}/manual_kage_disabled.c")
endif()

set(ISAAC_HOST_SOURCES
  main_linux.c
  platform_linux.c
  psp2_posix.c
  guest_window.c
  linux_stubs.c)

# Lua 5.3.3 (the game's Lua5.3.3r.dll) built natively; mods need it.
set(ISAAC_LUA53_SOURCE_DIR "" CACHE PATH "extracted lua-5.3.3 source root")
option(ISAAC_LUA "native Lua 5.3.3 bridge" ON)
if(ISAAC_LUA)
  if(NOT IS_DIRECTORY "${ISAAC_LUA53_SOURCE_DIR}/src")
    message(FATAL_ERROR "ISAAC_LUA needs ISAAC_LUA53_SOURCE_DIR (lua-5.3.3)")
  endif()
  include("${ISAAC_VITA}/lua53_vanilla.cmake")
  add_library(isaac_lua53 STATIC ${ISAAC_LUA53_C_SOURCES})
  target_include_directories(isaac_lua53 PUBLIC "${ISAAC_LUA53_SOURCE_DIR}/src")
  target_compile_options(isaac_lua53 PRIVATE -std=gnu11 -O2
    -fno-strict-aliasing)
  list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/host_vita_lua.c")
endif()

add_executable(isaac-linux64
  ${ISAAC_HOST_SOURCES}
  ${ISAAC_RUNTIME_SOURCES}
  ${ISAAC_GENERATED_C})

# Order matters: our platform.h shadows recomp/vita/platform.h.
target_include_directories(isaac-linux64 PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${ISAAC_RUNTIME}"
  "${ISAAC_VITA}"
  "${ISAAC_GENERATED_DIR}")

target_compile_definitions(isaac-linux64 PRIVATE
  __vita__=1
  GUEST_MEM_WINDOW=1
  GUEST_IMAGE_BASE=${ISAAC_GUEST_BASE}u
  GUEST_STACK_REQUIRED=1
  ISAAC_VITA_HAS_RUNTIME=1
  $<IF:$<BOOL:${ISAAC_LUA}>,ISAAC_VITA_LUA=1,ISAAC_VITA_LUA_OFF_CLOSE_NOOP=1>
  _GNU_SOURCE=1)

target_compile_options(isaac-linux64 PRIVATE
  -std=gnu11 -fno-strict-aliasing -fwrapv -Wno-builtin-declaration-mismatch)
set_source_files_properties(${ISAAC_GENERATED_C} PROPERTIES
  COMPILE_OPTIONS "-w")
set_source_files_properties(guest_window.c PROPERTIES
  COMPILE_OPTIONS "-Wno-unused-function")

# guest.c checks that the generated table matches the header it was built
# against; take the generation symbol from the table itself.
file(STRINGS "${ISAAC_GENERATED_DIR}/guest_table.c" ISAAC_GENERATION_LINE
  REGEX "guest_note_generation\\(guest_generation_[0-9a-f]+\\)" LIMIT_COUNT 1)
string(REGEX MATCH "guest_generation_[0-9a-f]+" ISAAC_GENERATION_SYMBOL
  "${ISAAC_GENERATION_LINE}")
if(NOT ISAAC_GENERATION_SYMBOL)
  message(FATAL_ERROR "no generation symbol in guest_table.c")
endif()
# Dense import-ID dispatch, as in the Vita production target: every IAT row
# (Lua included) resolves through host_vita_import_id.c.
set_property(SOURCE "${ISAAC_RUNTIME}/guest.c" APPEND PROPERTY
  COMPILE_DEFINITIONS GUEST_GENERATION_SYMBOL=${ISAAC_GENERATION_SYMBOL}
    ISAAC_VITA_IMPORT_ID_DISPATCH=1)

find_package(Threads REQUIRED)
target_link_libraries(isaac-linux64 PRIVATE Threads::Threads m)
if(ISAAC_SDL)
  target_link_libraries(isaac-linux64 PRIVATE SDL2::SDL2)
endif()
if(ISAAC_LUA)
  target_link_libraries(isaac-linux64 PRIVATE isaac_lua53)
endif()
