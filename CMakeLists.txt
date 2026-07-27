cmake_minimum_required(VERSION 3.10)
project(clog LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/include)

# Source files
set(CORE_SOURCES
    core/config.c
    core/formatter.c
    core/dispatcher.c
    core/queue.c
    core/async.c
    core/log.c
    core/rotate.c
)

set(SINK_SOURCES
    sinks/console_sink.c
    sinks/file_sink.c
    sinks/socket_sink.c
)

# Create static library
add_library(clog STATIC ${CORE_SOURCES} ${SINK_SOURCES})
target_link_libraries(clog Threads::Threads)

# Example executable
add_executable(example example/main.c)
target_link_libraries(example clog Threads::Threads)

# Set output directories
set_target_properties(clog PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    OUTPUT_NAME "clog"
)

set_target_properties(example PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

install(TARGETS clog ARCHIVE DESTINATION lib)
install(TARGETS example RUNTIME DESTINATION bin)
install(DIRECTORY include/ DESTINATION include/clog)
