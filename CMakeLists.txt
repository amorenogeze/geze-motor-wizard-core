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

# Los tests solo tienen sentido en build nativo (host); al cross-compilar
# contra el SDK de Yocto no hay entorno para correr GoogleTest en target.
if(NOT CMAKE_CROSSCOMPILING)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    # Windows/MSVC no aplica aquí, pero se mantiene por si se reutiliza el CMakeLists.
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    enable_testing()
    add_executable(wizard_tests
        tests/test_message.cpp
        tests/test_unix_socket.cpp
        tests/test_thread_safe_queue.cpp
    )
    target_link_libraries(wizard_tests PRIVATE shared gtest_main)
    include(GoogleTest)
    gtest_discover_tests(wizard_tests)
endif()

add_subdirectory(wizard-engine)
add_subdirectory(device-gateway)