# Build configuration for clogx-unused-includes clang-tidy check
cmake_minimum_required(VERSION 3.14)
project(clogx-unused-includes LANGUAGES CXX)

# Find LLVM/Clang packages
find_package(LLVM REQUIRED CONFIG)
find_package(Clang REQUIRED CONFIG)

message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION}")
message(STATUS "Using LLVMConfig.cmake in: ${LLVM_DIR}")
message(STATUS "Using ClangConfig.cmake in: ${Clang_DIR}")

# Add check as shared library
add_library(clogx-unused-includes SHARED
    UnusedIncludesCheck.cpp
)

# Include directories
target_include_directories(clogx-unused-includes PRIVATE
    ${LLVM_INCLUDE_DIRS}
    ${CLANG_INCLUDE_DIRS}
)

# Compile definitions
target_compile_definitions(clogx-unused-includes PRIVATE
    ${LLVM_DEFINITIONS}
)

# Link against Clang libraries
target_link_libraries(clogx-unused-includes PRIVATE
    clang-cpp
)

# Set output directory
set_target_properties(clogx-unused-includes PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/clang-tidy"
)
