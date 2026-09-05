cmake_minimum_required(VERSION 3.7)
project(Filters)

include(filters.cmake)

add_definitions(-fpic)

SET(LIBXMP_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/dec_xmp.c
)

SET(LIBXMP_LIBS
        ${CMAKE_CURRENT_SOURCE_DIR}/lib/libxmp.a
)

SET(LIBXMP_INC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

add_filter(libxmp
        "${LIBXMP_SRC}"
        "${LIBXMP_LIBS}"
        []
        ""
        "${LIBXMP_INC}"
        ""
        "1")
