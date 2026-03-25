cmake_minimum_required(VERSION 3.10)
project(SimpleChat)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Protobuf
find_package(Protobuf REQUIRED)

# Try finding ncurses or pdcurses
find_package(Curses REQUIRED)

# Ncurses header could be inside a subfolder in MSYS2/MinGW
find_path(NCURSES_INCLUDE_DIR NAMES ncurses.h PATH_SUFFIXES ncurses ncursesw include/ncurses)
if(NCURSES_INCLUDE_DIR)
    include_directories(${NCURSES_INCLUDE_DIR})
endif()

# Protobuf sources
file(GLOB PROTO_FILES "protos/*.proto")

# Generate Protobuf cpp sources
protobuf_generate_cpp(PROTO_SRCS PROTO_HDRS ${PROTO_FILES})

# Include directories
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}
    ${Protobuf_INCLUDE_DIRS}
    ${CURSES_INCLUDE_DIRS}
)

# Fix for Abseil in MSYS2 UCRT64 MSVC environments
if(MINGW)
    find_package(absl REQUIRED)
    set(ABSL_LIBS 
        absl::log_internal_log_sink_set
        absl::log_internal_message
        absl::log_internal_check_op
        absl::log_internal_globals
        absl::log_internal_conditions
        absl::log_internal_format
        absl::str_format_internal
        absl::log_internal_proto
        absl::log_internal_nullguard
        absl::synchronization
        absl::strings
        absl::time
        absl::base
    )
endif()

# Common utility sources if any, plus generated protos
set(COMMON_SRCS ${PROTO_SRCS} ${PROTO_HDRS})

# Server
add_executable(server src/server.cpp ${COMMON_SRCS})
if(MINGW)
    target_link_libraries(server protobuf::libprotobuf protobuf::libprotoc ${ABSL_LIBS})
else()
    target_link_libraries(server protobuf::libprotobuf protobuf::libprotoc)
endif()

# Client
add_executable(client src/client.cpp ${COMMON_SRCS})
if(MINGW)
    target_link_libraries(client protobuf::libprotobuf protobuf::libprotoc ${ABSL_LIBS} ${CURSES_LIBRARIES})
else()
    target_link_libraries(client protobuf::libprotobuf protobuf::libprotoc ${CURSES_LIBRARIES})
endif()

# platform-specific properties for Windows sockets
if(WIN32)
    target_link_libraries(server ws2_32)
    target_link_libraries(client ws2_32)
endif()
