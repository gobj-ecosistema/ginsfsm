##############################################
#   CMake
##############################################
project(ginsfsm C)
cmake_minimum_required(VERSION 2.8)
include(CheckIncludeFiles)
include(CheckSymbolExists)

set(CMAKE_INSTALL_PREFIX /yuneta/development/output)

set(INC_DEST_DIR ${CMAKE_INSTALL_PREFIX}/include)
set(LIB_DEST_DIR ${CMAKE_INSTALL_PREFIX}/lib)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -std=c99")

if(CMAKE_BUILD_TYPE MATCHES Debug)
  add_definitions(-DDEBUG)
  option(SHOWNOTES "Show preprocessor notes" OFF)

  if(CMAKE_COMPILER_IS_GNUCC)
    # GCC specific debug options
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O0 -g3 -ggdb3 -gdwarf-2")
    set(AVOID_VERSION -avoid-version)
  endif(CMAKE_COMPILER_IS_GNUCC)
endif(CMAKE_BUILD_TYPE MATCHES Debug)

add_definitions(-D_GNU_SOURCE)
add_definitions(-D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64)

check_include_files(regex.h HAVE_REGEX_H)
if(HAVE_REGEX_H)
    add_definitions(-DHAVE_REGEX_H)
else(HAVE_REGEX_H)
    message("***  WARNING: GNU C regex library not found.")
endif(HAVE_REGEX_H)

include_directories(BEFORE /yuneta/development/output/include)

##############################################
#   Source
##############################################

set (SRCS
    # GObj core, by order of dependency
    src/01_sdata.c
    src/10_gobj.c
    src/11_inter_event.c
    src/11_istream.c
    src/12_msg_ievent.c
    src/13_command_parser.c
    src/13_stats_parser.c
    src/ginsfsm.c
)


set (HDRS
    # GObj core, by order of dependency
    src/00_msglog_ginsfsm.h
    src/01_sdata.h
    src/10_gobj.h
    src/11_inter_event.h
    src/11_istream.h
    src/12_msg_ievent.h
    src/13_command_parser.h
    src/13_stats_parser.h
    src/ginsfsm.h
)

##############################################
#   Lib
##############################################
add_library(ginsfsm ${SRCS} ${HDRS})
set_target_properties(ginsfsm
    PROPERTIES COMPILE_DEFINITIONS "__SECURITY_AMICABLE_LEVEL__=1"
)


##############################################
#   System install
##############################################
install(FILES ${HDRS} DESTINATION ${INC_DEST_DIR})
install(
    TARGETS ginsfsm
    PERMISSIONS
    OWNER_READ OWNER_WRITE
    GROUP_READ GROUP_WRITE
    WORLD_READ
    DESTINATION ${LIB_DEST_DIR}
)
