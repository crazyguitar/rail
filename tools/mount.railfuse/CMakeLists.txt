add_executable(mount-railfuse main.cc options.cc)
target_link_libraries(mount-railfuse PRIVATE rail)
set_target_properties(mount-railfuse PROPERTIES OUTPUT_NAME mount.railfuse)
install(TARGETS mount-railfuse RUNTIME DESTINATION sbin)
