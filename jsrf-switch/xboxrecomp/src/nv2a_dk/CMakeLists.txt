cmake_minimum_required(VERSION 3.20)
project(nv2a_dk C)

# deko3d renderer for the pushbuffer executor (XBOXRECOMP_DEKO3D=ON), the
# Switch only: deko3d is the console's own GPU interface (devkitPro's
# deko3d package, part of switch-dev).
if(NOT NINTENDO_SWITCH)
    message(FATAL_ERROR "nv2a_dk (XBOXRECOMP_DEKO3D) builds for the Nintendo Switch only")
endif()

# dk_compile.c builds the shader pack on the console: dk_xbe_scan.c finds
# the game's shaders, dk_gen.c writes their GLSL (the GL renderer's
# generators, also used by tools/dk_pack) and uam compiles it.
add_library(xbox_nv2a_dk STATIC
    nv2a_dk.c
    dk_compile.c
    dk_gen.c
    dk_xbe_scan.c
)
target_include_directories(xbox_nv2a_dk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
option(XBOXRECOMP_DEKO3D_DEBUG "Link deko3d's checking library (deko3dd)" OFF)
if(XBOXRECOMP_DEKO3D_DEBUG)
    target_link_libraries(xbox_nv2a_dk PUBLIC uam deko3dd nx stdc++)
    target_compile_definitions(xbox_nv2a_dk PRIVATE NV2A_DK_CHECKED=1)
else()
    target_link_libraries(xbox_nv2a_dk PUBLIC uam deko3d nx stdc++)
endif()
target_compile_options(xbox_nv2a_dk PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)

# The shader pack's stamp (nv2a_dk_pack.h): a hash of the generators'
# sources, so a pack made by older ones is built anew. switch/dk_pack.sh
# works it out the same way for packs made on the PC.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/dk_gen_files.txt _dk_gen_lines)
set(_dk_gen_all "")
set(_dk_gen_deps ${CMAKE_CURRENT_SOURCE_DIR}/dk_gen_files.txt)
foreach(_f IN LISTS _dk_gen_lines)
    if(_f STREQUAL "" OR _f MATCHES "^#")
        continue()
    endif()
    file(SHA1 ${CMAKE_CURRENT_SOURCE_DIR}/${_f} _h)
    string(APPEND _dk_gen_all "${_h}")
    list(APPEND _dk_gen_deps ${CMAKE_CURRENT_SOURCE_DIR}/${_f})
endforeach()
string(SHA1 _dk_gen_all "${_dk_gen_all}")
string(SUBSTRING "${_dk_gen_all}" 0 16 DK_GEN_STAMP)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_dk_gen_deps})
target_compile_definitions(xbox_nv2a_dk PRIVATE DK_GEN_STAMP=0x${DK_GEN_STAMP}ull)
message(STATUS "deko3d shader generators: stamp ${DK_GEN_STAMP}")
