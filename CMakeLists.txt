cmake_minimum_required(VERSION 3.16)
IF (NOT ESP_PLATFORM)
PROJECT(GNUGo)
ELSE()
idf_component_register(
    REQUIRES esp_psram
     SRC_DIRS
     "engine"
     "sgf"
     "utils"
     "interface"
     "patterns"
    INCLUDE_DIRS
    "utils"
    "sgf"
    "engine"
    "patterns"
    "interface"
    ${BUILD_DIR}
    EXCLUDE_SRCS
    "interface/main.c"
    "interface/play_gtp.c"
    "interface/play_test.c"
    "patterns/mkpat.c"
    "patterns/mkeyes.c"
    "patterns/extract_fuseki.c"
    "patterns/compress_fuseki.c"
    "patterns/uncompress_fuseki.c"
    "patterns/mkmcpat.c"
) 
ENDIF()

INCLUDE(CheckIncludeFiles)
CHECK_INCLUDE_FILES(sys/times.h HAVE_SYS_TIMES_H)
CHECK_INCLUDE_FILES(sys/time.h HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILES("sys/time.h;time.h" TIME_WITH_SYS_TIME)
CHECK_INCLUDE_FILES(unistd.h HAVE_UNISTD_H)
CHECK_INCLUDE_FILES(curses.h HAVE_CURSES_H)
CHECK_INCLUDE_FILES(glib.h HAVE_GLIB_H)
CHECK_INCLUDE_FILES(ncurses/curses.h HAVE_NCURSES_CURSES_H)
CHECK_INCLUDE_FILES(ncurses/term.h HAVE_NCURSES_TERM_H)
CHECK_INCLUDE_FILES(sys/types.h HAVE_SYS_TYPES_H)
CHECK_INCLUDE_FILES(term.h HAVE_TERM_H)
CHECK_INCLUDE_FILES(crtdbg.h HAVE_CRTDBG_H)
CHECK_INCLUDE_FILES("winsock.h;io.h" HAVE_WINSOCK_IO_H)

INCLUDE(CheckFunctionExists)
CHECK_FUNCTION_EXISTS(times HAVE_TIMES)
CHECK_FUNCTION_EXISTS(usleep HAVE_USLEEP)
CHECK_FUNCTION_EXISTS(gettimeofday HAVE_GETTIMEOFDAY)
CHECK_FUNCTION_EXISTS(vsnprintf HAVE_VSNPRINTF)
CHECK_FUNCTION_EXISTS(_vsnprintf HAVE__VSNPRINTF)
# FIXME: Probably necessary to add the glib library for this test to pass.
CHECK_FUNCTION_EXISTS(g_vsnprintf HAVE_G_VSNPRINTF)

IF (ESP_PLATFORM)
SET(FIXED_BOARD_SIZE 9)
SET(SIZEOF_LONG 4)
component_compile_options(
    -Wno-misleading-indentation
    -Wno-stringop-overflow
    -Wno-overflow
    -Wno-bool-compare
    -Wno-logical-not-parentheses
    -Wno-analyzer-use-of-uninitialized-value
    -Wno-analyzer-possible-null-dereference
    -Wno-implicit-fallthrough
    -Wno-unused-but-set-variable
    -Wno-discarded-qualifiers
    -Wno-expansion-to-defined
    -Wno-old-style-definition
)
ADD_DEFINITIONS(-DHAVE_CONFIG_H)
# times.h is bad in picolibc
CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake
               ${BUILD_DIR}/config.h)
# # Run the pattern generators if not found
add_custom_command(OUTPUT patterns/patterns.c
    # Call CMake again directly - ugly but necessary because we need to
    # build the pattern generators for target architecture
    WORKING_DIRECTORY ${BUILD_DIR}
    COMMAND ${CMAKE_COMMAND} -E env "PATH=/usr/bin:/usr/local/bin:/bin" ${CMAKE_COMMAND} -B .build_patterns -DDISABLE_MONTE_CARLO=${CONFIG_DISABLE_MONTE_CARLO} -DFIXED_BOARD_SIZE=${FIXED_BOARD_SIZE} -DHAVE_CONFIG_H=1 -S ${COMPONENT_DIR} -Wno-dev
    COMMAND ${CMAKE_COMMAND} -E env "PATH=/usr/bin:/usr/local/bin:/bin" ${CMAKE_COMMAND} --build .build_patterns -t patterns -j
    COMMAND ${CMAKE_COMMAND} -E copy .build_patterns/patterns/*.c ${CMAKE_CURRENT_SOURCE_DIR}/patterns
    COMMENT "Generated pattern database."
)
add_custom_target(gen_patterns DEPENDS patterns/patterns.c)
add_dependencies(${COMPONENT_LIB} gen_patterns)
idf_build_set_property(MINIMAL_BUILD ON)
ELSE()

INCLUDE(CheckTypeSize)
CHECK_TYPE_SIZE(long SIZEOF_LONG)

SET(PRAGMAS "")
IF(WIN32)
    SET(PRAGMAS "#pragma warning(disable: 4244 4305)")
ENDIF(WIN32)

IF(MSVC80)
    ADD_DEFINITIONS(-D_CRT_SECURE_NO_DEPRECATE)
    ADD_DEFINITIONS(-D_CRT_NONSTDC_NO_DEPRECATE)
ENDIF(MSVC80)

CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/config.h.cmake
               ${CMAKE_CURRENT_BINARY_DIR}/config.h)

# Make sure all files know about and can find config.h
ADD_DEFINITIONS(-DHAVE_CONFIG_H)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR})

# Recurse into subdirectories.
ADD_SUBDIRECTORY(utils)
ADD_SUBDIRECTORY(sgf)
ADD_SUBDIRECTORY(engine)
ADD_SUBDIRECTORY(patterns)
ADD_SUBDIRECTORY(interface)
ENDIF()
