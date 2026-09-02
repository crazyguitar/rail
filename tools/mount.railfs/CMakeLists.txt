add_executable(mount-railfs main.c)
set_target_properties(mount-railfs PROPERTIES OUTPUT_NAME mount.railfs)
install(TARGETS mount-railfs RUNTIME DESTINATION sbin)
