cmake_minimum_required(VERSION 3.16)
project(wizard-core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)

# JSON parser for device profiles (device-gateway/canopen/devices/*.json).
# Yocto: add nlohmann-json to DEPENDS so the system package is found and
# nothing is downloaded at build time.
find_package(nlohmann_json 3.2 QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(nlohmann_json
        URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
    FetchContent_MakeAvailable(nlohmann_json)
endif()

add_library(shared shared/message.cpp shared/unix_socket.cpp shared/db.cpp)
target_include_directories(shared PUBLIC shared)
target_link_libraries(shared PUBLIC Threads::Threads SQLite::SQLite3)

add_subdirectory(wizard-engine)
add_subdirectory(device-gateway)

if(NOT CMAKE_CROSSCOMPILING)
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
    enable_testing()
    add_executable(wizard_tests
        tests/test_message.cpp
        tests/test_unix_socket.cpp
        tests/test_thread_safe_queue.cpp
        tests/test_device_profile.cpp
        tests/test_commands.cpp
        tests/test_canopen_translator.cpp)
    target_link_libraries(wizard_tests PRIVATE shared gateway gtest_main)
    target_compile_definitions(wizard_tests PRIVATE
        WIZARD_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    include(GoogleTest)
    gtest_discover_tests(wizard_tests)
endif()
