add_executable(raild-cli main.cc)
set_target_properties(raild-cli PROPERTIES OUTPUT_NAME raild)
target_link_libraries(raild-cli PRIVATE rail)
install(TARGETS raild-cli RUNTIME DESTINATION bin)
