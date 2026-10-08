include(GNUInstallDirs)

# CANopen protocol + device profile: knows addresses, nothing about UI commands.
add_library(canopen STATIC
    canopen/canopen_client.cpp
    canopen/canopen_translator.cpp
    canopen/device_profile.cpp)
target_include_directories(canopen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(canopen PUBLIC shared nlohmann_json::nlohmann_json)

# UI commands + the wizard-engine connection. Shared by the gateway and the tests.
add_library(gateway STATIC
    commands/commands.cpp
    engine_session.cpp)
target_link_libraries(gateway PUBLIC canopen)

add_executable(device-gateway main.cpp)
target_link_libraries(device-gateway PRIVATE gateway)

# Installed profile location, compiled in as the default lookup path.
set(WIZARD_DEVICE_PROFILE_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/wizard/devices")
set(WIZARD_COMMANDS_DIR "${CMAKE_INSTALL_FULL_SYSCONFDIR}/wizard/commands")
target_compile_definitions(device-gateway PRIVATE
    WIZARD_DEFAULT_DEVICE_PROFILE="${WIZARD_DEVICE_PROFILE_DIR}/solopico.json"
    WIZARD_DEFAULT_COMMANDS="${WIZARD_COMMANDS_DIR}/solopico_commands.json")

# Native runs from the build tree find <exe dir>/devices/solopico.json and
# <exe dir>/commands/solopico_commands.json.
add_custom_command(TARGET device-gateway POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            ${CMAKE_CURRENT_SOURCE_DIR}/canopen/devices $<TARGET_FILE_DIR:device-gateway>/devices
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:device-gateway>/commands
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/commands/solopico_commands.json $<TARGET_FILE_DIR:device-gateway>/commands/)

install(TARGETS device-gateway RUNTIME DESTINATION bin)
install(DIRECTORY canopen/devices/ DESTINATION ${WIZARD_DEVICE_PROFILE_DIR})
install(DIRECTORY commands/ DESTINATION ${WIZARD_COMMANDS_DIR} FILES_MATCHING PATTERN "*.json")  # not the .cpp/.h next to it
