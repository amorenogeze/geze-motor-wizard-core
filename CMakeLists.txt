cmake_minimum_required(VERSION 3.16)
project(wizard-backend CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_library(shared STATIC
    shared/message.cpp
    shared/unix_socket.cpp
)
target_include_directories(shared PUBLIC shared)
target_link_libraries(shared PUBLIC Threads::Threads)

# Tests only make sense in native builds; there is no environment to run
# GoogleTest on target when cross-compiling.
if(NOT CMAKE_CROSSCOMPILING)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(wizard_tests
        tests/test_message.cpp
        tests/test_unix_socket.cpp
    )
    target_link_libraries(wizard_tests PRIVATE shared gtest_main)
    include(GoogleTest)
    gtest_discover_tests(wizard_tests)
endif()

add_subdirectory(wizard-engine)
add_subdirectory(device-gateway)