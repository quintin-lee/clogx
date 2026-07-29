cmake_minimum_required(VERSION 3.14)
project(clogx VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(CLOG_BUILD_EXAMPLES "Build example programs" ON)
option(CLOG_BUILD_TESTS "Build and register tests" ON)
option(CLOG_BUILD_BENCHMARKS "Build benchmark programs" OFF)
option(CLOG_BUILD_SHARED "Build shared library instead of static" OFF)
option(CLOG_USE_SYSTEM_YAML "Use system libyaml instead of auto-downloading" OFF)
option(CLOG_ENABLE_TLS "Enable OpenSSL TLS support for socket sink" OFF)
option(CLOG_ENABLE_CLANG_TIDY "Enable clang-tidy static analysis during compilation" OFF)

if(CLOG_ENABLE_CLANG_TIDY)
    find_program(CLANG_TIDY_EXE NAMES "clang-tidy")
    if(CLANG_TIDY_EXE)
        set(CMAKE_C_CLANG_TIDY "${CLANG_TIDY_EXE}")
        message(STATUS "clang-tidy enabled: ${CLANG_TIDY_EXE}")
    else()
        message(WARNING "CLOG_ENABLE_CLANG_TIDY is ON but clang-tidy executable was not found")
    endif()
endif()

find_package(Threads REQUIRED)

if(CLOG_ENABLE_TLS)
    find_package(OpenSSL REQUIRED)
endif()

if(CLOG_USE_SYSTEM_YAML)
    find_package(yaml CONFIG QUIET)
    if(TARGET yaml)
        set(CLOG_YAML_TARGET yaml)
    elseif(TARGET YAML::YAML)
        set(CLOG_YAML_TARGET YAML::YAML)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(YAML REQUIRED yaml-0.1)
        set(CLOG_YAML_TARGET ${YAML_LIBRARIES})
    endif()
else()
    set(YAML_PREFER_CONFIG OFF)
    if(NOT TARGET yaml)
        include(FetchContent)
        FetchContent_Declare(
            yaml
            URL https://codeload.github.com/yaml/libyaml/tar.gz/refs/tags/0.2.5
            URL_HASH SHA256=fa240dbf262be053f3898006d502d514936c818e422afdcf33921c63bed9bf2e
        )
        FetchContent_GetProperties(yaml)
        if(NOT yaml_POPULATED)
            set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
            set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
            FetchContent_Populate(yaml)
            add_subdirectory(${yaml_SOURCE_DIR} ${yaml_BINARY_DIR} EXCLUDE_FROM_ALL)
        endif()
    endif()
    set(CLOG_YAML_TARGET yaml)
endif()

set(CLOG_SOURCES
    core/config.c
    core/formatter.c
    core/dispatcher.c
    core/queue.c
    core/async.c
    core/log.c
    core/rotate.c
    core/rate_limit.c
    core/signal_handler.c
    sinks/console_sink.c
    sinks/file_sink.c
    sinks/socket_sink.c
    sinks/custom_sink.c
    sinks/syslog_sink.c
)

# Object library — all sources compiled once, shared across library and internal-symbol tests
add_library(clogx_objects OBJECT ${CLOG_SOURCES})
target_include_directories(clogx_objects
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
target_compile_definitions(clogx_objects PRIVATE _GNU_SOURCE)
target_compile_options(clogx_objects
    PRIVATE
        $<$<C_COMPILER_ID:GNU,Clang,AppleClang>:-Wall -Wextra -Wconversion -fvisibility=hidden>
)
target_link_libraries(clogx_objects PUBLIC Threads::Threads ${CLOG_YAML_TARGET})
if(WIN32)
    target_link_libraries(clogx_objects PUBLIC ws2_32)
endif()
if(CLOG_ENABLE_TLS)
    target_compile_definitions(clogx_objects PRIVATE CLOG_USE_TLS)
    target_link_libraries(clogx_objects PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()
if(CLOG_USE_SYSTEM_YAML)
    target_include_directories(clogx_objects SYSTEM PUBLIC ${YAML_INCLUDE_DIRS})
    target_compile_options(clogx_objects PUBLIC ${YAML_CFLAGS_OTHER})
endif()
set_target_properties(clogx_objects PROPERTIES POSITION_INDEPENDENT_CODE ON)

# Main library (shared or static)
if(CLOG_BUILD_SHARED)
    add_library(clogx SHARED $<TARGET_OBJECTS:clogx_objects>)
else()
    add_library(clogx STATIC $<TARGET_OBJECTS:clogx_objects>)
endif()

add_library(clogx::clogx ALIAS clogx)

target_include_directories(clogx
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

set_target_properties(clogx PROPERTIES
    OUTPUT_NAME clogx
    VERSION ${PROJECT_VERSION}
    SOVERSION 0
)

target_link_libraries(clogx PUBLIC Threads::Threads yaml)
if(WIN32)
    target_link_libraries(clogx PUBLIC ws2_32)
endif()
if(CLOG_ENABLE_TLS)
    target_link_libraries(clogx PUBLIC OpenSSL::SSL OpenSSL::Crypto)
endif()

# --- Example ---
if(CLOG_BUILD_EXAMPLES)
    add_executable(clog_example example/main.c)
    target_link_libraries(clog_example PRIVATE clogx)
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
        test_pipeline
        verify_config
        test_invalid_config
        test_double_init
        test_empty_sink
        test_async_fallback
        test_queue_try_put
        test_max_size
        test_console_stderr
        test_module_trunc
        test_mutex_guard_raii
        test_add_sink
        test_multithread_sync
        test_config_set
        test_boundary_config
        test_socket_sink
        test_sink_level
        test_log_level
        test_json_formatter
        test_rate_limit
        test_fork_safety
        test_signal_handler
        test_custom_sink
        test_observability_stats
        test_syslog_sink
        test_thread_context
        test_coverage_boost
    )

    # Tests that directly reference internal symbols (not exported from shared lib)
    set(CLOG_INTERNAL_TESTS test_queue_try_put test_async_fallback test_async_reload)

    foreach(test_name IN LISTS CLOG_TEST_SOURCES)
        if(CLOG_BUILD_SHARED AND test_name IN_LIST CLOG_INTERNAL_TESTS)
            # Link objects directly so internal symbols are available
            add_executable(${test_name} tests/${test_name}.c $<TARGET_OBJECTS:clogx_objects>)
            target_include_directories(${test_name} PRIVATE
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
            target_link_libraries(${test_name} PRIVATE Threads::Threads)
        else()
            add_executable(${test_name} tests/${test_name}.c)
            target_link_libraries(${test_name} PRIVATE clogx)
        endif()
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

if(CLOG_BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif()

# --- Install / export ---
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

install(TARGETS clogx
    EXPORT clogxTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(FILES
    include/log.h
    include/log_config.h
    include/log_limits.h
    include/log_record.h
    include/log_sink.h
    include/clog_port.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/clogx
)

if(CLOG_BUILD_EXAMPLES)
    install(TARGETS clog_example
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()

install(EXPORT clogxTargets
    FILE clogxTargets.cmake
    NAMESPACE clogx::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/clogx
)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clogx-config.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/clogx-config.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/clogx
)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/clogx-config-version.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/clogx-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/clogx-config-version.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/clogx
)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/clogx.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/clogx.pc
    @ONLY
)

install(FILES ${CMAKE_CURRENT_BINARY_DIR}/clogx.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

# --- CPack Configuration ---
set(CPACK_PACKAGE_NAME "clogx")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Lightweight C99 logging library")
set(CPACK_PACKAGE_VENDOR "quintin-lee")
set(CPACK_PACKAGE_LICENSE "MIT")
set(CPACK_GENERATOR "TGZ;ZIP;DEB;RPM")
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "quintin-lee")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libyaml-0-2 (>= 0.2.5), libpthread")
set(CPACK_RPM_PACKAGE_REQUIRES "libyaml >= 0.2.5")

include(CPack)

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

# --- Clang-Tidy Target ---
find_program(CLANG_TIDY_EXECUTABLE NAMES "clang-tidy")
if(CLANG_TIDY_EXECUTABLE)
    add_custom_target(tidy
        COMMAND ${CLANG_TIDY_EXECUTABLE} -p ${CMAKE_BINARY_DIR} ${CLOG_SOURCES}
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "Running clang-tidy static analysis on clogx sources"
        VERBATIM
    )
endif()
