cmake_minimum_required(VERSION 3.20)

# Jet Set Radio Future (Xbox, NTSC-U), statically recompiled for the
# Nintendo Switch. Configure with devkitPro's Switch toolchain; switch/build.sh
# does it all.

project(jsrf_recomp C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "jsrf_recomp builds for the Nintendo Switch only: configure with "
                        "devkitPro's Switch toolchain (see switch/build.sh)")
endif()

# The runtime's sources use the Win32 vocabulary (platform/xbox_winnt.h).
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp" CACHE PATH
    "The recompilation runtime")
set(JSRF_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH
    "The lifted JSRF code (recomp_*.c, recomp_funcs.h), generated from your default.xbe")
if(NOT EXISTS "${JSRF_GEN_DIR}/recomp_funcs.h")
    message(FATAL_ERROR "No generated code in ${JSRF_GEN_DIR} (see its README).")
endif()

# The SD folder the build keeps its log, settings, saves and shader cache in,
# sdmc:/switch/<name>. The game files are always read from
# sdmc:/switch/jsrf/game, so a side-by-side build (JSRF_HOME_NAME=jsrf-test)
# shares them and leaves the main install's settings, saves and shaders alone.
set(JSRF_HOME_NAME "jsrf" CACHE STRING "SD folder name: sdmc:/switch/<name>")
add_compile_definitions(JSRF_SD_HOME="sdmc:/switch/${JSRF_HOME_NAME}")

# The renderer is deko3d (xboxrecomp/src/nv2a_dk). The console compiles its
# shader pack with uam, which is C++ (xboxrecomp/third_party/uam).
# JSRF_DEKO3D_DEBUG links deko3d's checking library: a misuse is named in the
# log and on screen instead of ending the program with a bare 0x367. It costs
# CPU on every call, so it is for chasing a deko3d error.
option(JSRF_DEKO3D_DEBUG "Link deko3d's checking library (deko3dd)" OFF)
set(XBOXRECOMP_DEKO3D ON CACHE BOOL "" FORCE)
set(XBOXRECOMP_DEKO3D_DEBUG ${JSRF_DEKO3D_DEBUG} CACHE BOOL "" FORCE)

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

# ── The program ──────────────────────────────────────────────────────────

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${JSRF_GEN_DIR}/*.c")
add_executable(${PROJECT_NAME}
    src/main.c
    src/switch_nx.c
    src/recomp_manual.c
    src/jsrf_manual_overrides.c
    src/adx_guard.c
    src/xiso_extract.c
    ${RECOMP_GEN_SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${JSRF_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
    "$ENV{DEVKITPRO}/portlibs/switch/include")
target_compile_definitions(${PROJECT_NAME} PRIVATE RECOMP_MMIO_RUNTIME=1 JSRF_DEKO3D=1)
set_property(SOURCE src/switch_nx.c APPEND PROPERTY
    INCLUDE_DIRECTORIES "${XBOXRECOMP_DIR}/src/apu")
target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp nx m)

# switch_nx.c steps in front of two library calls: SDL's audio thread
# priority, and libnx's audio buffer submit (its timing, for the report).
target_link_options(${PROJECT_NAME} PRIVATE
    "LINKER:--wrap=SDL_SYS_SetThreadPriority"
    "LINKER:--wrap=audrvVoiceAddWaveBuf")

# ── The lifted code's compiler flags ─────────────────────────────────────
#
# -w: it is machine-made. -fno-strict-aliasing and -fwrapv: x86 semantics.
# -ffp-contract=off: a multiply and an add stay two roundings, as on the
# Xbox (SSE has no fused multiply-add and the x87 rounds each step), now that
# the values are in registers the compiler could fuse
# (regen/jsrf_fp_locals.py, regen/jsrf_simd_locals.py).
set(JSRF_GEN_OPT "-O2" CACHE STRING "Optimisation for the lifted code")
set(JSRF_GEN_FLAGS "${JSRF_GEN_OPT};-w;-fno-strict-aliasing;-fwrapv;-ffp-contract=off")

# Thread-local guest registers reached without the compiler's TLS call
# (recomp_types.h, FAST THREAD-LOCAL REGISTERS); OFF: plain __thread.
option(JSRF_FAST_TLS "Lifted code: thread-local registers without the TLS call" ON)
if(NOT JSRF_FAST_TLS)
    list(APPEND JSRF_GEN_FLAGS "-DRECOMP_NO_FAST_TLS")
endif()

# The preemption checks regen/jsrf_preempt.py adds at every function entry
# and loop back edge. Nothing needs them now that guest threads run in
# parallel; built without them the game's code is 13% smaller and XDK D3D's
# 5%.
option(JSRF_PREEMPT_POINTS "Lifted code: RECOMP_PREEMPT checks" OFF)
if(NOT JSRF_PREEMPT_POINTS)
    list(APPEND JSRF_GEN_FLAGS "-DRECOMP_NO_PREEMPT_POINTS")
endif()
set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES COMPILE_OPTIONS "${JSRF_GEN_FLAGS}")

# ── The homebrew package ─────────────────────────────────────────────────

set(JSRF_SWITCH_APP_NAME "Jet Set Radio Future" CACHE STRING "Title in the homebrew menu")
set(JSRF_SWITCH_AUTHOR "Smilebit / SEGA" CACHE STRING "Author in the homebrew menu")
set(JSRF_SWITCH_VERSION "1.0.0" CACHE STRING "Version in the homebrew menu")
# The icon: your own 256x256 JPEG at assets/icon.jpg if you put one there
# (it stays out of the repository), else the port's, assets/jsrf_icon.jpg.
set(JSRF_SWITCH_ICON "" CACHE FILEPATH "256x256 JPEG icon; empty: assets/icon.jpg, else assets/jsrf_icon.jpg")
if(NOT JSRF_SWITCH_ICON)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
        set(JSRF_SWITCH_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg")
    else()
        set(JSRF_SWITCH_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/jsrf_icon.jpg")
    endif()
endif()

set(JSRF_NACP "${CMAKE_CURRENT_BINARY_DIR}/jsrf.nacp")
nx_generate_nacp(OUTPUT "${JSRF_NACP}"
    NAME "${JSRF_SWITCH_APP_NAME}"
    AUTHOR "${JSRF_SWITCH_AUTHOR}"
    VERSION "${JSRF_SWITCH_VERSION}")
if(JSRF_SWITCH_ICON)
    nx_create_nro(${PROJECT_NAME} NACP "${JSRF_NACP}" ICON "${JSRF_SWITCH_ICON}")
else()
    nx_create_nro(${PROJECT_NAME} NACP "${JSRF_NACP}")
endif()
