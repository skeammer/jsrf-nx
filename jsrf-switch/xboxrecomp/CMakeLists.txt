cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.9.0 LANGUAGES C)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (147 imports → Win32)
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a_dk   - the deko3d renderer for the pushbuffer executor

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Line tables in the runtime's objects on the Switch, so the sampling
# profiler's report can name the line a hot function was on (LINES= in
# switch/prof_report.sh). -g1 changes no code, and the NRO carries no debug
# sections; only the ELF grows.
if(NINTENDO_SWITCH)
    add_compile_options(-g1)
endif()

# Option to build examples

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
add_subdirectory(src/platform)

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

option(XBOXRECOMP_DEKO3D "Build the deko3d renderer (nv2a_dk, Switch only)" OFF)

# Audio: DirectSound → software mixer compatibility
add_subdirectory(src/audio)

# Audio: MCPX APU hardware emulation (voice processor, DSP)
add_subdirectory(src/apu)

# USB: MCPX OHCI host controllers. A title's XAPI is statically linked and
# drives these registers directly, so a pad only exists if they do.
add_subdirectory(src/usb)

# Input: Xbox gamepad → XInput
add_subdirectory(src/input)

# Renderer for the pushbuffer executor: deko3d, on the Switch.
if(XBOXRECOMP_DEKO3D)
    # uam, the GLSL -> DKSH compiler, for the console's own shader pack
    enable_language(CXX)
    add_subdirectory(third_party/uam)
    add_subdirectory(src/nv2a_dk)
endif()

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
add_library(xboxrecomp INTERFACE)
target_link_libraries(xboxrecomp INTERFACE
    xbox_kernel
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_apu
)
if(XBOXRECOMP_DEKO3D)
    target_link_libraries(xboxrecomp INTERFACE xbox_nv2a_dk)
endif()

