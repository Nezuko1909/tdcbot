cmake_minimum_required(VERSION 3.16)

project(dcbot VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Enable compile_commands.json for clangd / VS Code
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Output executable to build/bin
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Project include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Find D++ installed on the system
find_package(DPP REQUIRED)
find_package(CURL REQUIRED)

# Collect project source files
file(GLOB_RECURSE BOT_SOURCES
    "src/commands/*.cpp"
    "src/events/*.cpp"
    "src/handlers/*.cpp"
    "src/utils/*.cpp"
    "src/config/*.cpp"
)

# Create executable
add_executable(dcbot
    src/main.cpp
    ${BOT_SOURCES}
)

# Project-specific include directories
target_include_directories(dcbot PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
)

# Link system-installed D++ and libcurl
target_link_libraries(dcbot PRIVATE
    dpp
    CURL::libcurl
)

# Debug build definition
target_compile_definitions(dcbot PRIVATE
    $<$<CONFIG:Debug>:DEBUG_BUILD>
)

# Compiler warnings
target_compile_options(dcbot PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:Clang>:-Wall;-Wextra;-Wpedantic>
    $<$<CXX_COMPILER_ID:MSVC>:/W4>
)

# Tests
if(BUILD_TESTING)
    enable_testing()

    find_package(Catch2 3 REQUIRED)

    add_executable(dcbot_tests
        tests/test_main.cpp
        ${BOT_SOURCES}
    )

    target_link_libraries(dcbot_tests PRIVATE
        Catch2::Catch2WithMain
        dpp
        CURL::libcurl
    )

    add_test(
        NAME dcbot_tests
        COMMAND dcbot_tests
    )
endif()
