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
set(ISAAC_WRAP_SYMBOLS)

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
  if(NINTENDO_SWITCH)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ISAAC_NX_SDL2 REQUIRED IMPORTED_TARGET sdl2)
    add_library(SDL2::SDL2 ALIAS PkgConfig::ISAAC_NX_SDL2)
  else()
    find_package(SDL2 REQUIRED)
  endif()
  list(APPEND ISAAC_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/manual_kage.c"
    "${ISAAC_RUNTIME}/gl_pc_backend.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../sdl/kage_sdl_backend.c")
else()
  list(APPEND ISAAC_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/kage_pc_disabled.c"
    "${ISAAC_RUNTIME}/manual_kage_disabled.c")
endif()

# MSVC x86 C++ exceptions (runtime/guest_cxx_eh.c): throw, catch, unwind
# funclets and catch continuations.  Needs a corpus generated with
# GUEST_CXX_EH=1 (recomp/cxx_eh.py); the Vita target keeps it off.
option(ISAAC_CXX_EH "MSVC C++ exception handling (GUEST_CXX_EH)" ON)
file(STRINGS "${ISAAC_GENERATED_DIR}/guest_funcs.h" ISAAC_CXX_EH_CORPUS
  REGEX "^#define GUEST_CXX_EH_CORPUS 1$" LIMIT_COUNT 1)
if(ISAAC_CXX_EH)
  if(NOT ISAAC_CXX_EH_CORPUS)
    message(FATAL_ERROR
      "ISAAC_CXX_EH needs a corpus generated with GUEST_CXX_EH=1 "
      "(GUEST_CXX_EH=1 python3 recomp/gen_all.py ...); "
      "or configure with -DISAAC_CXX_EH=OFF")
  endif()
  list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/guest_cxx_eh.c")
endif()

if(NINTENDO_SWITCH)
  # Same host layer on libnx/newlib; ../switch adds the entry point and the
  # ux0:/app0: path devices over sdmc:/switch/isaac.
  set(ISAAC_HOST_SOURCES
    ../switch/main_switch.c
    ../switch/switch_paths.c
    ../switch/switch_malloc.c
    ../switch/switch_sampler.c
    ../switch/switch_bg.c
    ../switch/switch_clock.c
    platform_linux.c
    mod_probe_cache.c
    psp2_posix.c
    guest_window.c)
else()
  set(ISAAC_HOST_SOURCES
    main_linux.c
    platform_linux.c
    mod_probe_cache.c
    psp2_posix.c
    guest_window.c
    linux_stubs.c)
endif()

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

# Lua fast paths from the Vita release (recomp/vita/CMakeLists.txt): the
# per-frame GC step clamp (the game asks for up to 10 MB per frame with a mod
# loaded), one bounded step instead of the per-floor full collection, typed
# endpoints for the hottest Lua imports, and native LuaBridge __index /
# Userdata::getClass+getExact replays (translated fallback on any deviation).
set(ISAAC_LUA_GCSTEP_CLAMP_KB 128 CACHE STRING
  "per-frame lua_gc(LUA_GCSTEP) clamp, KB (0 = the game's own step)")
option(ISAAC_LUA_GCCOLLECT_CLAMP
  "floor-transition GCCOLLECT -> one bounded step" ON)
set(ISAAC_LUA_GCCOLLECT_STEP_KB 256 CACHE STRING
  "KB paid by the clamped floor-transition collection")
option(ISAAC_LUA_FASTPATHS
  "Lua typed import endpoints and native __index/getClass seams" ON)
if(ISAAC_LUA)
  if(ISAAC_LUA_GCSTEP_CLAMP_KB GREATER 0)
    set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_lua.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_LUA_GCSTEP_CLAMP_KB=${ISAAC_LUA_GCSTEP_CLAMP_KB})
  endif()
  if(ISAAC_LUA_GCCOLLECT_CLAMP)
    list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/host_vita_lua_gc.c")
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_lua.c"
      "${ISAAC_RUNTIME}/host_vita_lua_gc.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_LUA_GCCOLLECT_CLAMP=1
        ISAAC_VITA_LUA_GCCOLLECT_STEP_KB=${ISAAC_LUA_GCCOLLECT_STEP_KB})
  endif()
  if(ISAAC_LUA_FASTPATHS)
    list(APPEND ISAAC_RUNTIME_SOURCES
      "${ISAAC_RUNTIME}/host_vita_lua_getclass.c")
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_lua.c"
      "${ISAAC_RUNTIME}/host_vita_import_id.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LUA_IMPORT_FASTDISPATCH=1)
    set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_lua.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LUA_NATIVE_INDEX=1)
    set_property(SOURCE
      "${ISAAC_RUNTIME}/host_vita_lua.c"
      "${ISAAC_RUNTIME}/host_vita_lua_getclass.c"
      APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_LUA_NATIVE_GETCLASS=1)
    set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_lua_getclass.c"
      APPEND PROPERTY COMPILE_DEFINITIONS
        ISAAC_VITA_LUA_NATIVE_GETCLASS_WRAP=1
        ISAAC_VITA_LUA_NATIVE_GETCLASS_BUILD_ID=\"linux64\")
    list(APPEND ISAAC_WRAP_SYMBOLS sub_003f8e30 sub_003f8c80)
  endif()
endif()

# Audio: the shared OpenAL adapter (host_vita_audio.c) over system OpenAL
# Soft.  Only its owners see ISAAC_VITA_AUDIO, as in the Vita target.
option(ISAAC_AUDIO "OpenAL audio" ON)
if(ISAAC_AUDIO)
  if(NOT ISAAC_SDL)
    message(FATAL_ERROR "ISAAC_AUDIO needs ISAAC_SDL (manual_kage.c)")
  endif()
  if(NINTENDO_SWITCH)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(ISAAC_NX_OPENAL REQUIRED IMPORTED_TARGET openal)
    add_library(OpenAL::OpenAL ALIAS PkgConfig::ISAAC_NX_OPENAL)
  else()
    find_package(OpenAL REQUIRED)
  endif()
  list(APPEND ISAAC_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/host_vita_audio.c"
    "${ISAAC_RUNTIME}/host_vita_audio_cooperative.c")
  set_property(SOURCE
    "${ISAAC_RUNTIME}/entry_vita.c"
    "${ISAAC_RUNTIME}/host_vita_first_fault.c"
    "${ISAAC_RUNTIME}/host_vita_import_id.c"
    "${ISAAC_RUNTIME}/host_vita_audio_cooperative.c"
    "${ISAAC_RUNTIME}/manual_kage.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_AUDIO=1)
endif()

# The game's own controller path (libstem XInput) over a synthetic XInput
# module; the SDL backend supplies the pad snapshot.
option(ISAAC_XINPUT "Synthetic XInput gamepad" ON)
if(ISAAC_XINPUT)
  if(NOT ISAAC_SDL)
    message(FATAL_ERROR "ISAAC_XINPUT needs ISAAC_SDL (kage_sdl_backend.c)")
  endif()
  list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/host_vita_xinput.c")
  set_property(SOURCE
    "${ISAAC_RUNTIME}/host_vita_first_fault.c"
    "${ISAAC_RUNTIME}/host_vita_import_id.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_XINPUT=1)
endif()

# ---------------------------------------------------------------------------
# Native fast paths ported from the Vita target (recomp/vita/CMakeLists.txt,
# where each option is documented).  They replace hot translated library code
# (libpng, zlib, stb_vorbis, ...) with native code at link-time --wrap seams
# or generator-emitted entry seams; every seam keeps the translated body as
# its fallback.  The NEON variants are selected only on AArch64 (Switch); the
# scalar code paths are used elsewhere.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" OR NINTENDO_SWITCH)
  set(ISAAC_HOST_NEON ON)
else()
  set(ISAAC_HOST_NEON OFF)
endif()

include("${ISAAC_VITA}/generated_definitions.cmake")
# The generated source that defines `symbol` (exactly one), for seams that
# need a compile definition on their generated owner.
function(isaac_generated_owner symbol output)
  set(owners)
  foreach(source IN LISTS ISAAC_GENERATED_C)
    isaac_generated_definition_lines("${source}" match LIMIT_COUNT 2
      REGEX "^void ${symbol}\\(CPU \\*__restrict c\\)$")
    if(match)
      list(APPEND owners "${source}")
    endif()
  endforeach()
  list(LENGTH owners count)
  if(NOT count EQUAL 1)
    message(FATAL_ERROR "${symbol} must have one generated owner; found ${count}")
  endif()
  set(${output} "${owners}" PARENT_SCOPE)
endfunction()

# Exact-range heap leases (host_vita_heap.c) for the seams that run native
# code on guest heap objects.
set(ISAAC_HEAP_LEASE_FEATURES)

# Packed archives (.a): the miniz v1.15 inflate coroutine (0x5aeb00) behind
# its generated entry seam, and the per-block word XOR (0x5b06f0, wrapped)
# with its ISAAC PRNG refresh.  Every resource read during loads and room
# transitions goes through both.
option(ISAAC_ARCHIVE_MINIZ_FASTPATH
  "Native archive inflate (ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH)" ON)
option(ISAAC_ARCHIVE_XOR_FASTPATH
  "Native archive XOR + PRNG refresh (ISAAC_VITA_ARCHIVE_XOR_FASTPATH)" ON)
if(ISAAC_ARCHIVE_MINIZ_FASTPATH)
  isaac_generated_owner(sub_005aeb00 ISAAC_ARCHIVE_MINIZ_OWNER)
  list(APPEND ISAAC_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/host_vita_archive_miniz_guest.c")
  set_property(SOURCE ${ISAAC_ARCHIVE_MINIZ_OWNER}
    "${ISAAC_RUNTIME}/host_vita_archive_miniz_guest.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH=1)
  list(APPEND ISAAC_HEAP_LEASE_FEATURES ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH=1)
  set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_archive_miniz_guest.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_HEAP_RANGE_LEASE=1
      ISAAC_VITA_ARCHIVE_STACK_OBJECTS=1)
endif()
if(ISAAC_ARCHIVE_XOR_FASTPATH)
  list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/host_vita_archive_xor.c")
  set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_archive_xor.c"
    APPEND PROPERTY COMPILE_DEFINITIONS
      ISAAC_VITA_ARCHIVE_XOR_FASTPATH=1
      ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH=1
      ISAAC_VITA_ARCHIVE_STACK_OBJECTS=1
      ISAAC_VITA_HEAP_RANGE_LEASE=1)
  list(APPEND ISAAC_HEAP_LEASE_FEATURES ISAAC_VITA_ARCHIVE_XOR_FASTPATH=1)
  list(APPEND ISAAC_WRAP_SYMBOLS sub_005b06f0)
endif()
# Native PNG: png_read_row (0x5b1500) is wrapped; whole-image inflate and
# unfilter natively into a reserved scratch.
option(ISAAC_NATIVE_PNG "Native PNG row decode (ISAAC_VITA_NATIVE_PNG)" ON)
set(ISAAC_NATIVE_PNG_RESERVE_MB 16 CACHE STRING
  "MiB of PNG scratch reserved at start (whole-image staging)")
option(ISAAC_NATIVE_PNG_REUSE
  "Reuse decoded rows of identical PNGs (ISAAC_VITA_NATIVE_PNG_REUSE)" ON)
set(ISAAC_NATIVE_PNG_REUSE_MB 4 CACHE STRING "MiB for decoded PNG reuse (1..8)")
if(ISAAC_NATIVE_PNG)
  list(APPEND ISAAC_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/host_vita_native_png.c"
    "${ISAAC_RUNTIME}/host_vita_native_png_adler.c"
    "${ISAAC_RUNTIME}/host_vita_native_png_miniz.c"
    "${ISAAC_RUNTIME}/host_vita_native_png_libdeflate.c")
  set(ISAAC_NP_DEFS
    ISAAC_VITA_NATIVE_PNG=1
    ISAAC_VITA_NATIVE_PNG_RESERVE_MB=${ISAAC_NATIVE_PNG_RESERVE_MB}
    ISAAC_VITA_NATIVE_PNG_BUILD_ID=\"linux64\"
    ISAAC_VITA_NATIVE_PNG_ADLER_NEON=1
    ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT=1
    ISAAC_VITA_NATIVE_PNG_FIXED_TABLES=1
    ISAAC_VITA_NATIVE_PNG_TINFL_HEADER_RESET=1
    ISAAC_VITA_NATIVE_PNG_CLOCK_ELISION=1)
  if(ISAAC_HOST_NEON)
    list(APPEND ISAAC_NP_DEFS
      ISAAC_VITA_NATIVE_PNG_RGBA_NEON=1
      ISAAC_VITA_NATIVE_PNG_LZ_NEON=1
      ISAAC_VITA_NATIVE_PNG_LZ_D4_NEON=1)
  endif()
  if(ISAAC_NATIVE_PNG_REUSE)
    list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/host_vita_png_reuse.c")
    list(APPEND ISAAC_NP_DEFS
      ISAAC_VITA_NATIVE_PNG_REUSE=1
      ISAAC_VITA_NATIVE_PNG_REUSE_TINFL=1
      ISAAC_VITA_NATIVE_PNG_REUSE_MB=${ISAAC_NATIVE_PNG_REUSE_MB})
  endif()
  set_property(SOURCE
    "${ISAAC_RUNTIME}/host_vita_native_png.c"
    "${ISAAC_RUNTIME}/host_vita_native_png_adler.c"
    "${ISAAC_RUNTIME}/host_vita_native_png_miniz.c"
    "${ISAAC_RUNTIME}/host_vita_native_png_libdeflate.c"
    "${ISAAC_RUNTIME}/host_vita_png_reuse.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ${ISAAC_NP_DEFS})
  set_property(SOURCE "${ISAAC_RUNTIME}/entry_vita.c"
    APPEND PROPERTY COMPILE_DEFINITIONS
      ISAAC_VITA_NATIVE_PNG=1
      ISAAC_VITA_NATIVE_PNG_RESERVE_MB=${ISAAC_NATIVE_PNG_RESERVE_MB})
  list(APPEND ISAAC_WRAP_SYMBOLS sub_005b1500)
endif()

# CRT: keep one idle read-only packed-archive FILE open for the next open
# of the same archive (every resource load reopens its .a), and answer the
# game's SEEK_END/ftell size probes on read-only streams from a cached size.
option(ISAAC_ARCHIVE_FILE_CACHE
  "Reuse an idle packed-archive FILE (ISAAC_VITA_ARCHIVE_FILE_CACHE)" ON)
option(ISAAC_CRT_SEEK_SHADOW
  "Cached-size SEEK_END/ftell on read-only FILEs (ISAAC_VITA_CRT_SEEK_SHADOW)" ON)
if(ISAAC_CRT_SEEK_SHADOW AND NOT ISAAC_ARCHIVE_FILE_CACHE)
  message(FATAL_ERROR "ISAAC_CRT_SEEK_SHADOW requires ISAAC_ARCHIVE_FILE_CACHE")
endif()
if(ISAAC_ARCHIVE_FILE_CACHE)
  list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/host_vita_archive_cache.c")
  set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_crt.c"
    APPEND PROPERTY COMPILE_DEFINITIONS
      ISAAC_VITA_ARCHIVE_FILE_CACHE=1
      ISAAC_VITA_ARCHIVE_DIAG_BUILD_ID=\"linux64\")
endif()
if(ISAAC_CRT_SEEK_SHADOW)
  set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_crt.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_CRT_SEEK_SHADOW=1)
endif()

# The game rewrites gamestateN.dat (~68 KB) on every room change through a
# 1 KiB-buffered FILE: dozens of synchronous writes plus open/truncate/close
# inside Game::Update, a visible hitch on an SD card.  Hand whole save images
# to one writer thread instead (ISAAC_VITA_ASYNC_SAVE_WRITE).
option(ISAAC_ASYNC_SAVE_WRITE
  "Write Documents/*.dat saves from a worker thread (ISAAC_VITA_ASYNC_SAVE_WRITE)" ON)
if(ISAAC_ASYNC_SAVE_WRITE AND NOT ISAAC_ARCHIVE_FILE_CACHE)
  message(FATAL_ERROR "ISAAC_ASYNC_SAVE_WRITE requires ISAAC_ARCHIVE_FILE_CACHE")
endif()
if(ISAAC_ASYNC_SAVE_WRITE)
  list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/host_vita_async_write.c")
  set_property(SOURCE
    "${ISAAC_RUNTIME}/host_vita_crt.c"
    "${ISAAC_RUNTIME}/host_vita_filesystem.c"
    "${ISAAC_RUNTIME}/host_vita_startup.c"
    "${ISAAC_RUNTIME}/host_vita_find.c"
    "${ISAAC_RUNTIME}/host_vita_async_write.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_ASYNC_SAVE_WRITE=1)
  set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_async_write.c"
    APPEND PROPERTY COMPILE_DEFINITIONS
      ISAAC_VITA_ASYNC_WRITE_BUILD_ID=\"linux64\")
endif()

# libpng chunk CRC-32 byte loop (0x5c3320) natively, behind its generated
# entry seam.
option(ISAAC_PNG_CRC32_FASTPATH
  "Native PNG chunk CRC-32 (ISAAC_VITA_PNG_CRC32_FASTPATH)" ON)
if(ISAAC_PNG_CRC32_FASTPATH)
  isaac_generated_owner(sub_005c3320 ISAAC_PNG_CRC32_OWNER)
  list(APPEND ISAAC_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/host_vita_png_crc32_native.c"
    "${ISAAC_RUNTIME}/host_vita_png_crc32_guest.c")
  set_property(SOURCE ${ISAAC_PNG_CRC32_OWNER}
    "${ISAAC_RUNTIME}/host_vita_png_crc32_guest.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_PNG_CRC32_FASTPATH=1)
endif()

# Startup: skip re-validating packed-archive entries whose index checksum
# matches the one baked into the PE (the generated owner keeps the original
# validation for everything else).
option(ISAAC_ARCHIVE_VALIDATION_SKIP
  "Skip PE-baked archive checksum matches (ISAAC_VITA_ARCHIVE_VALIDATION_SKIP)" ON)
if(ISAAC_ARCHIVE_VALIDATION_SKIP)
  isaac_generated_owner(sub_00563880 ISAAC_ARCHIVE_VALIDATION_OWNER)
  set_property(SOURCE ${ISAAC_ARCHIVE_VALIDATION_OWNER}
    APPEND PROPERTY COMPILE_DEFINITIONS
      ISAAC_VITA_ARCHIVE_VALIDATION_SKIP=1
      ISAAC_VITA_ARCHIVE_VALIDATION_SKIP_BUILD_ID=\"linux64\")
endif()

# Sprite-sheet retention: wrap the ImageManager release observer
# (0x56d8c0) so a sheet whose last sprite died stays resident on a
# byte-budgeted LRU; the next Load of the same name (tears, the next room)
# reuses the uploaded texture instead of re-reading, decoding and uploading.
option(ISAAC_IMAGE_RETAIN "Retain released sprite sheets (ISAAC_VITA_IMAGE_RETAIN)" ON)
set(ISAAC_IMAGE_RETAIN_BUDGET_MB 48 CACHE STRING
  "MiB of retained sprite-sheet texels before the LRU evicts")
if(ISAAC_IMAGE_RETAIN)
  list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/host_vita_image_retain.c")
  set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_image_retain.c"
    APPEND PROPERTY COMPILE_DEFINITIONS
      ISAAC_VITA_IMAGE_RETAIN=1
      ISAAC_VITA_IMAGE_RETAIN_WRAP=1
      ISAAC_VITA_IMAGE_RETAIN_LAZY_ARM=1
      ISAAC_VITA_IMAGE_RETAIN_BUDGET_MB=${ISAAC_IMAGE_RETAIN_BUDGET_MB}
      ISAAC_VITA_IMAGE_RETAIN_HEADROOM_MB=0
      ISAAC_VITA_IMAGE_RETAIN_BUILD_ID=\"linux64\")
  list(APPEND ISAAC_WRAP_SYMBOLS sub_0056d8c0)
endif()

# Native stb_vorbis (host_native_vorbis64.c): the game's OGG decoder
# (stb_vorbis v1.04, ~10-20x slower translated) runs natively for whole-sample
# SFX loads and music streams.  Unlike the Vita module it never clones the
# guest decoder state: each translated open is mirrored by a native open over
# the same bytes (guest memory, or the KAGE stream through guest_call), and
# the wrapped get_samples / seek_start / deinit serve the mirror.
option(ISAAC_NATIVE_VORBIS "Native stb_vorbis mirrors for OGG decoding" ON)
if(ISAAC_NATIVE_VORBIS)
  if(NOT ISAAC_AUDIO)
    message(FATAL_ERROR "ISAAC_NATIVE_VORBIS needs ISAAC_AUDIO")
  endif()
  list(APPEND ISAAC_RUNTIME_SOURCES "${ISAAC_RUNTIME}/host_native_vorbis64.c")
  # IEEE single-precision steps as the PE's SSE code does them (no FMA
  # contraction on AArch64), so Linux and Switch decode the same PCM.
  set_property(SOURCE "${ISAAC_RUNTIME}/host_native_vorbis64.c"
    APPEND PROPERTY COMPILE_OPTIONS -ffp-contract=off)
  set_property(SOURCE "${ISAAC_RUNTIME}/entry_vita.c"
    APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_NATIVE_VORBIS64=1)
  list(APPEND ISAAC_WRAP_SYMBOLS
    sub_005bdf80   # stb_vorbis_get_samples_short_interleaved
    sub_005b7740   # set_file_offset (Decode's inlined seek_start)
    sub_005bb260   # vorbis_pump_first_frame
    sub_005bd310   # vorbis_deinit
    sub_005bd740   # stb_vorbis_open_file (KAGE stream edit)
    sub_005bd8b0)  # stb_vorbis_open_memory
endif()

if(ISAAC_ARCHIVE_MINIZ_FASTPATH OR ISAAC_NATIVE_PNG)
  # tinfl core shared by the archive seam and the native PNG decoder.
  list(APPEND ISAAC_RUNTIME_SOURCES
    "${ISAAC_RUNTIME}/host_vita_archive_miniz_native.c")
endif()

if(ISAAC_HEAP_LEASE_FEATURES)
  set_property(SOURCE "${ISAAC_RUNTIME}/host_vita_heap.c"
    APPEND PROPERTY COMPILE_DEFINITIONS
      ISAAC_VITA_HEAP_RANGE_LEASE=1 ${ISAAC_HEAP_LEASE_FEATURES})
endif()

# Frame-time spike log: times present-to-present through a link-time wrap
# of SDL_GL_SwapWindow and logs every frame above ISAAC_FRAME_SPIKE_MS.
option(ISAAC_FRAME_SPIKE_LOG "Log frame-time spikes (diagnostic)" OFF)
set(ISAAC_FRAME_SPIKE_MS 25 CACHE STRING "frame length logged as a spike, ms")
if(ISAAC_FRAME_SPIKE_LOG)
  if(NOT ISAAC_SDL)
    message(FATAL_ERROR "ISAAC_FRAME_SPIKE_LOG needs ISAAC_SDL")
  endif()
  list(APPEND ISAAC_HOST_SOURCES frame_spike_log.c)
  set_property(SOURCE frame_spike_log.c APPEND PROPERTY COMPILE_DEFINITIONS
    ISAAC_FRAME_SPIKE_MS=${ISAAC_FRAME_SPIKE_MS})
endif()

add_executable(isaac-linux64
  ${ISAAC_HOST_SOURCES}
  ${ISAAC_RUNTIME_SOURCES}
  ${ISAAC_GENERATED_C})

# Order matters: our platform.h shadows recomp/vita/platform.h.
target_include_directories(isaac-linux64 PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/../switch"
  "${CMAKE_CURRENT_SOURCE_DIR}/compat"
  "${ISAAC_RUNTIME}"
  "${ISAAC_VITA}"
  "${ISAAC_GENERATED_DIR}")

target_compile_definitions(isaac-linux64 PRIVATE
  __vita__=1
  GUEST_MEM_WINDOW=1
  GUEST_IMAGE_BASE=${ISAAC_GUEST_BASE}u
  GUEST_STACK_REQUIRED=1
  $<$<BOOL:${ISAAC_CXX_EH}>:GUEST_CXX_EH=1>
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
# Indirect calls and import slots resolve by binary search per call
# (guest_lookup + guest_import_lookup were ~10% of slow frames on the
# Switch); memoize them (ISAAC_VITA_GUEST_LOOKUP_CACHE, owner: guest.c).
option(ISAAC_GUEST_LOOKUP_CACHE
  "Cache guest_lookup / import slot lookups (ISAAC_VITA_GUEST_LOOKUP_CACHE)" ON)
if(ISAAC_GUEST_LOOKUP_CACHE)
  set_property(SOURCE "${ISAAC_RUNTIME}/guest.c" APPEND PROPERTY
    COMPILE_DEFINITIONS ISAAC_VITA_GUEST_LOOKUP_CACHE=1)
endif()

if(ISAAC_FRAME_SPIKE_LOG)
  target_link_options(isaac-linux64 PRIVATE -Wl,--wrap=SDL_GL_SwapWindow)
endif()
foreach(ISAAC_WRAP_SYMBOL IN LISTS ISAAC_WRAP_SYMBOLS)
  target_link_options(isaac-linux64 PRIVATE -Wl,--wrap=${ISAAC_WRAP_SYMBOL})
endforeach()

find_package(Threads REQUIRED)
target_link_libraries(isaac-linux64 PRIVATE Threads::Threads m)
if(ISAAC_SDL)
  target_link_libraries(isaac-linux64 PRIVATE SDL2::SDL2)
endif()
if(ISAAC_LUA)
  target_link_libraries(isaac-linux64 PRIVATE isaac_lua53)
endif()
if(ISAAC_AUDIO)
  target_link_libraries(isaac-linux64 PRIVATE OpenAL::OpenAL)
endif()

if(NINTENDO_SWITCH)
  # SDL2's Switch port (mesa) and OpenAL Soft are C++: link with the C++
  # driver so libstdc++ comes last.
  enable_language(CXX)
  set_target_properties(isaac-linux64 PROPERTIES LINKER_LANGUAGE CXX)
  set_target_properties(isaac-linux64 PROPERTIES OUTPUT_NAME isaac)
  nx_generate_nacp(isaac.nacp
    NAME "The Binding of Isaac: Repentance"
    AUTHOR "repentogxm port"
    VERSION "1.7.9b")
  nx_create_nro(isaac-linux64 OUTPUT isaac.nro NACP isaac.nacp NOICON)
endif()
