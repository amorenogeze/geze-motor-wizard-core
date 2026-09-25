include(GNUInstallDirs)

# CANopen protocol + device profile code, shared by the gateway and the tests.
add_library(canopen STATIC
    canopen/canopen_client.cpp
    canopen/canopen_translator.cpp
    canopen/device_profile.cpp)
target_include_directories(canopen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(canopen PUBLIC shared nlohmann_json::nlohmann_json)

add_executable(device-gateway main.cpp)
target_link_libraries(device-gateway PRIVATE canopen)

# Installed profile location, compiled in as the default lookup path.
set(WIZARD_DEVICE_PROFILE_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/wizard/devices")
target_compile_definitions(device-gateway PRIVATE
    WIZARD_DEFAULT_DEVICE_PROFILE="${WIZARD_DEVICE_PROFILE_DIR}/solopico.json")

# Native runs from the build tree find <exe dir>/devices/solopico.json.
add_custom_command(TARGET device-gateway POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/canopen/devices $<TARGET_FILE_DIR:device-gateway>/devices)

install(TARGETS device-gateway RUNTIME DESTINATION bin)
install(DIRECTORY canopen/devices/ DESTINATION ${WIZARD_DEVICE_PROFILE_DIR})
