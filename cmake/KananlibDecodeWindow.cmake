# Keep the pinned dependency untouched, including shared FetchContent overrides.
# Only exhaustive_decode's readable-window check is overlaid in the build tree.
if(NOT WIN32 OR NOT TARGET kananlib)
    message(FATAL_ERROR "The Kananlib decode-window backport requires the Windows kananlib target")
endif()

set(_uevr_kanan_header "${kananlib_SOURCE_DIR}/include/utility/Scan.hpp")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_uevr_kanan_header}")
file(READ "${_uevr_kanan_header}" _uevr_kanan_original)
string(REPLACE "\r\n" "\n" _uevr_kanan_original "${_uevr_kanan_original}")
string(SHA256 _uevr_kanan_hash "${_uevr_kanan_original}")
if(NOT _uevr_kanan_hash STREQUAL "3be8c9189e5c7db544f3efdb081b3b6918f07d05b8a79ac780b7f4727afae1b3")
    message(FATAL_ERROR "Kananlib Scan.hpp differs from pinned 49fdf2d; review the decode-window backport before changing this dependency")
endif()

set(_uevr_kanan_old [=[                // This instead of IsBadReadPtr so we don't branch into kernel32 every time
                // we want to test the readability of the memory
#ifdef NDEBUG
                __try {
                    volatile auto test1 = *(uintptr_t*)(ip);
                    volatile auto test8 = *(uintptr_t*)(ip + 56); // check if we can read ahead without page crossing
                    (void)test1; (void)test8;
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    break;
                }
#else
                if (IsBadReadPtr(ip, 64)) {
                    break;
                }
#endif
                const auto status = NdDecodeEx(&ctx.instrux, ip, 64, ND_CODE_64, ND_DATA_64);]=])
set(_uevr_kanan_new [=[                // Decode readable instructions even at the end of a mapping.
                const auto readable = utility::uevr_kananlib_backport::readable_decode_window(ip);
                if (readable == 0) {
                    break;
                }
                const auto status = NdDecodeEx(&ctx.instrux, ip, readable, ND_CODE_64, ND_DATA_64);]=])
string(FIND "${_uevr_kanan_original}" "${_uevr_kanan_old}" _uevr_kanan_location)
if(_uevr_kanan_location EQUAL -1)
    message(FATAL_ERROR "Kananlib decode-window replacement anchor is missing")
endif()
string(REPLACE "${_uevr_kanan_old}" "${_uevr_kanan_new}" _uevr_kanan_patched "${_uevr_kanan_original}")
string(REPLACE "#include <utility/Benchmark.hpp>" "#include <utility/Benchmark.hpp>\n#include <utility/uevr/ReadableDecodeWindow.hpp>"
    _uevr_kanan_patched "${_uevr_kanan_patched}")

set(_uevr_kanan_overlay "${CMAKE_CURRENT_BINARY_DIR}/kananlib-decode-window/include")
file(MAKE_DIRECTORY "${_uevr_kanan_overlay}/utility/uevr")
# Sibling quoted includes (notably Emulation.hpp -> "Scan.hpp") must resolve to
# the same header copy as angle includes; otherwise #pragma once cannot help.
file(GLOB_RECURSE _uevr_kanan_headers CONFIGURE_DEPENDS
    RELATIVE "${kananlib_SOURCE_DIR}/include" "${kananlib_SOURCE_DIR}/include/*")
foreach(_uevr_kanan_relative IN LISTS _uevr_kanan_headers)
    if(NOT _uevr_kanan_relative STREQUAL "utility/Scan.hpp")
        configure_file("${kananlib_SOURCE_DIR}/include/${_uevr_kanan_relative}"
            "${_uevr_kanan_overlay}/${_uevr_kanan_relative}" COPYONLY)
    endif()
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/../dependencies/kananlib/ReadableDecodeWindow.hpp"
    "${_uevr_kanan_overlay}/utility/uevr/ReadableDecodeWindow.hpp" COPYONLY)
file(GENERATE OUTPUT "${_uevr_kanan_overlay}/utility/Scan.hpp" CONTENT "${_uevr_kanan_patched}")
foreach(_uevr_kanan_target IN ITEMS kananlib kananlib-nolog)
    if(TARGET ${_uevr_kanan_target})
        target_include_directories(${_uevr_kanan_target} BEFORE PUBLIC "${_uevr_kanan_overlay}")
    endif()
endforeach()
message(STATUS "Kananlib: isolated dcb698e readable-window backport (pinned source unchanged)")
