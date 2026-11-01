# uam -- devkitPro's GLSL -> deko3d (DKSH) shader compiler (https://github.com/
# devkitPro/uam, 1.1.0, fincs; Mesa's GLSL front end and nouveau codegen), as
# a static library so the deko3d renderer can build its shader pack on the
# console (nv2a_dk/dk_compile.c, uam.h).
#
# Vendored from its source tree with the files its meson build generates
# (generated/: the GLSL and glcpp lexers and parsers, made with flex/bison,
# and the ir_expression_operation headers, made by Python), so this builds
# with nothing but a C/C++ toolchain. Changes for jsrf-nx are marked
# "jsrf-nx": the front end kept between compiles, the codegen's buffers
# freed, DKSH output to memory, and uam.h. The command-line driver
# (main.cpp) is not built. Licences: LICENSE (uam) and Mesa's own headers.

include(${CMAKE_CURRENT_SOURCE_DIR}/sources.cmake)

add_library(uam STATIC ${UAM_SOURCES})
target_include_directories(uam PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/generated
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/mesa-imported
    ${CMAKE_CURRENT_SOURCE_DIR}/mesa-imported
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/mesa-imported/glsl
    ${CMAKE_CURRENT_SOURCE_DIR}/generated/mesa-imported/glsl/glcpp)
target_include_directories(uam PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/source)
target_compile_definitions(uam PRIVATE
    NDEBUG "PACKAGE_STRING=\"uam 1.1.0\"" DESKTOP _USE_MATH_DEFINES _GNU_SOURCE
    HAVE_POSIX_MEMALIGN)
set_target_properties(uam PROPERTIES
    C_STANDARD 99 C_STANDARD_REQUIRED ON C_EXTENSIONS ON
    CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS ON
    POSITION_INDEPENDENT_CODE ON)
# Upstream's flags; warnings off (vendored code), optimised even in debug
# builds -- a slow compiler is a slow first start. This Mesa is from before
# GCC 14/15 made some old C and C++ leniencies errors and trimmed libstdc++'s
# internal includes: -fpermissive keeps those warnings (silenced by -w), and
# <cstdint> is included up front for the C++ files.
target_compile_options(uam PRIVATE -ffunction-sections -w -O2 -fpermissive
    "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdint>")

# The Switch build links the system's Mesa too (libEGL.a, through SDL2):
# every symbol this Mesa defines gets a uamx_ prefix (private_syms.cmake),
# so the two never resolve to each other.
# The binutils beside the compiler (aarch64-none-elf-nm ... for the Switch:
# the host's would not read its objects), else CMake's.
get_filename_component(_cxx_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
get_filename_component(_cxx_name "${CMAKE_CXX_COMPILER}" NAME)
string(REGEX REPLACE "(g\\+\\+|c\\+\\+|clang\\+\\+)(-[0-9.]+)?(\\.exe)?$" "" _prefix "${_cxx_name}")
foreach(_tool NM OBJCOPY READELF RANLIB)
    string(TOLOWER ${_tool} _exe)
    set(UAM_${_tool} "")
    if(EXISTS "${_cxx_dir}/${_prefix}${_exe}")
        set(UAM_${_tool} "${_cxx_dir}/${_prefix}${_exe}")
    elseif(CMAKE_${_tool})
        set(UAM_${_tool} "${CMAKE_${_tool}}")
    else()
        find_program(UAM_${_tool}_FOUND NAMES ${_prefix}${_exe} ${_exe})
        if(UAM_${_tool}_FOUND)
            set(UAM_${_tool} "${UAM_${_tool}_FOUND}")
        endif()
    endif()
endforeach()
if(NOT UAM_NM OR NOT UAM_OBJCOPY)
    message(FATAL_ERROR "uam: nm and objcopy are needed to make its symbols private")
endif()
message(STATUS "uam: symbols made private with ${UAM_OBJCOPY}")
add_custom_command(TARGET uam POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DNM=${UAM_NM} -DOBJCOPY=${UAM_OBJCOPY}
            -DREADELF=${UAM_READELF} -DRANLIB=${UAM_RANLIB} -DLIB=$<TARGET_FILE:uam>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/private_syms.cmake
    VERBATIM)
