add_executable(mount-railnfs main.cc serve.cc helper.cc)
target_link_libraries(mount-railnfs PRIVATE rail)
set_target_properties(mount-railnfs PROPERTIES OUTPUT_NAME mount.railnfs)
install(TARGETS mount-railnfs RUNTIME DESTINATION sbin)

add_executable(umount-railnfs umount.cc helper.cc)
set_target_properties(umount-railnfs PROPERTIES OUTPUT_NAME umount.railnfs)
install(TARGETS umount-railnfs RUNTIME DESTINATION sbin)
