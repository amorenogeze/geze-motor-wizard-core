add_executable(wizard-engine main.cpp)
target_link_libraries(wizard-engine PRIVATE shared)
install(TARGETS wizard-engine RUNTIME DESTINATION bin)
