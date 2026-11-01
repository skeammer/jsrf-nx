# private_syms.cmake -- give every symbol libuam.a defines a uamx_ prefix,
# except uam.h's API, so this Mesa never meets another one at link time.
#
# The Switch build also links the system's Mesa (devkitPro's libEGL.a, which
# SDL2 pulls in): the same names -- glsl_type::vec4_type, ralloc_context,
# _mesa_hash_table_create, ... -- from another Mesa version. The linker then
# reported multiple definitions, and worse, where it took a symbol from the
# library it met first, uam's code would have called the other Mesa's
# functions with its own structures. Renamed, both copies are private.
#
#   cmake -DNM=<nm> -DOBJCOPY=<objcopy> -DRANLIB=<ranlib> -DLIB=<libuam.a> \
#         -P private_syms.cmake
#
# Definitions and references are renamed together in every member (one map),
# so uam still links with itself. Undefined symbols (libc, libstdc++) are
# left alone. COMDAT groups are renamed too: an inline function's group is
# named after it, but GCC puts a class's constructors and destructors in a
# group named after a local symbol (..C5Ev, ..D5Ev) -- left as it was, the
# linker kept the other Mesa's group of the same name and dropped uam's, and
# uam's vtables pointed at nothing (a crash in the first compile). Group
# names come from readelf -g (and nm's local 'n' symbols).
# Idempotent: names that already carry the prefix are skipped.

foreach(v NM OBJCOPY LIB)
    if(NOT ${v})
        message(FATAL_ERROR "private_syms.cmake: ${v} not set")
    endif()
endforeach()

execute_process(COMMAND ${NM} -P --defined-only -g ${LIB}
    OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "private_syms.cmake: ${NM} failed: ${_err}")
endif()

set(_names "")
macro(_take _name)
    if(NOT "${_name}" STREQUAL "" AND NOT "${_name}" MATCHES "^(uamx_|[.$])"
       AND NOT "${_name}" STREQUAL "uam_compile_dksh" AND NOT "${_name}" STREQUAL "uam_shutdown")
        list(APPEND _names "${_name}")
    endif()
endmacro()

# Every global symbol uam defines: "name type value size"; archive members
# as "lib.a[x.o]:".
string(REPLACE "\n" ";" _lines "${_out}")
foreach(_l IN LISTS _lines)
    if(_l STREQUAL "" OR _l MATCHES ":$")
        continue()
    endif()
    string(REGEX MATCH "^[^ ]+" _name "${_l}")
    _take("${_name}")
endforeach()

# COMDAT group names that are local symbols (GCC's C5/D5 groups).
execute_process(COMMAND ${NM} -P -a --defined-only ${LIB}
    OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "private_syms.cmake: ${NM} -a failed: ${_err}")
endif()
string(REPLACE "\n" ";" _lines "${_out}")
foreach(_l IN LISTS _lines)
    if(_l MATCHES "^([^ ]+) [nN] ")
        _take("${CMAKE_MATCH_1}")
    endif()
endforeach()
if(READELF)
    execute_process(COMMAND ${READELF} -gW ${LIB}
        OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    string(REPLACE "\n" ";" _lines "${_out}")
    foreach(_l IN LISTS _lines)
        if(_l MATCHES "COMDAT group section .*\\[([^]]+)\\] contains")
            _take("${CMAKE_MATCH_1}")
        endif()
    endforeach()
endif()
# A weak or COMDAT symbol is defined in many members; objcopy takes each
# old name once.
list(REMOVE_DUPLICATES _names)
list(LENGTH _names _n)
set(_map "")
foreach(_name IN LISTS _names)
    string(APPEND _map "${_name} uamx_${_name}\n")
endforeach()

get_filename_component(_dir ${LIB} DIRECTORY)
set(_mapfile ${_dir}/uam_private_syms.map)
file(WRITE ${_mapfile} "${_map}")
if(_n GREATER 0)
    execute_process(COMMAND ${OBJCOPY} --redefine-syms=${_mapfile} ${LIB}
        RESULT_VARIABLE _rc ERROR_VARIABLE _err)
    if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "private_syms.cmake: ${OBJCOPY} failed: ${_err}")
    endif()
    if(RANLIB)
        execute_process(COMMAND ${RANLIB} ${LIB} RESULT_VARIABLE _rc)
    endif()
endif()
message(STATUS "uam: ${_n} symbols made private (uamx_)")
