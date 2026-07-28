add_executable(device-gateway main.cpp canopen/canopen_client.cpp)
target_link_libraries(device-gateway PRIVATE shared)