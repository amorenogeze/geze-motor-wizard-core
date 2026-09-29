add_executable(device-gateway main.cpp canopen/canopen_client.cpp canopen/canopen_translator.cpp)
target_link_libraries(device-gateway PRIVATE shared)
install(TARGETS device-gateway RUNTIME DESTINATION bin)
