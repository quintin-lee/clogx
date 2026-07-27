cmake_minimum_required(VERSION 3.14)
project(clog VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CLOG_BUILD_EXAMPLES "Build example programs" ON)
option(CLOG_BUILD_TESTS "Build and register tests" ON)
option(CLOG_BUILD_SHARED "Build shared library instead of static" OFF)

find_package(Threads REQUIRED)

set(CLOG_SOURCES
    core/config.c
    core/formatter.c
    core/dispatcher.c
    core/queue.c
    core/async.c
    core/log.c
    core/rotate.c
    sinks/console_sink.c
    sinks/file_sink.c
    sinks/socket_sink.c
)

if(CLOG_BUILD_SHARED)
    add_library(clog SHARED ${CLOG_SOURCES})
else()
    add_library(clog STATIC ${CLOG_SOURCES})
endif()

add_library(clog::clog ALIAS clog)

target_include_directories(clog
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_compile_definitions(clog
    PRIVATE
        _GNU_SOURCE
)

target_compile_options(clog
    PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra>
)

target_link_libraries(clog
    PUBLIC
        Threads::Threads
)

set_target_properties(clog PROPERTIES
    OUTPUT_NAME clog
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
    POSITION_INDEPENDENT_CODE ON
)

# --- Example ---
if(CLOG_BUILD_EXAMPLES)
    add_executable(clog_example example/main.c)
    target_link_libraries(clog_example PRIVATE clog)
    set_target_properties(clog_example PROPERTIES
        OUTPUT_NAME example
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
    )
endif()

# --- Tests ---
if(CLOG_BUILD_TESTS)
    enable_testing()

    set(CLOG_TEST_SOURCES
        test_async_lifecycle
        test_async_reload
        test_dispatcher_lifecycle
        test_file_rotate
        test_file_mkdir
        test_config_reload
    )

    foreach(test_name IN LISTS CLOG_TEST_SOURCES)
        add_executable(${test_name} tests/${test_name}.c)
        target_link_libraries(${test_name} PRIVATE clog)
        target_compile_definitions(${test_name} PRIVATE _GNU_SOURCE)
        set_target_properties(${test_name} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
        )
        add_test(
            NAME ${test_name}
            COMMAND ${test_name}
            WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        )
    endforeach()
endif()

# --- Install / export ---
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS clog
    EXPORT clogTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(DIRECTORY include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/clog
    FILES_MATCHING PATTERN "*.h"
)

if(CLOG_BUILD_EXAMPLES)
    install(TARGETS clog_example
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(EXPORT clogTargets
    FILE clogTargets.cmake
    NAMESPACE clog::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/clog
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clog-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/clog-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/clog
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/clog-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/clog-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/clog-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/clog
)

# --- Docs (optional; requires doxygen) ---
find_program(DOXYGEN_EXECUTABLE doxygen)
if(DOXYGEN_EXECUTABLE)
    add_custom_target(docs
        COMMAND ${DOXYGEN_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/Doxyfile
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Generating API documentation with Doxygen"
        VERBATIM
    )
endif()
