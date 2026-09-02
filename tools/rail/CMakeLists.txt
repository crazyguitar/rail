add_executable(rail-cli main.cc)
set_target_properties(rail-cli PROPERTIES OUTPUT_NAME rail)
target_link_libraries(rail-cli PRIVATE rail)
install(TARGETS rail-cli RUNTIME DESTINATION bin)
