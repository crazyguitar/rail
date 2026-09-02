add_executable(rail-e2e
  harness.cc
  privileged.cc
  root-check.cc
  test-version.cc
  test-basic.cc
  test-pull.cc
  test-kernel.cc
  test-delta.cc
  test-memory.cc
  test-pages.cc
  test-edge.cc
  test-failure.cc
  test-throughput.cc
  test-directory.cc
  test-tree.cc
  test-naming.cc
  test-filter.cc
  test-service.cc
  test-nfs.cc
  test-fuse.cc
  test-loop.cc
  test-uring.cc
)
target_link_libraries(rail-e2e PRIVATE rail-testing GTest::gtest_main)
add_dependencies(rail-e2e rail-cli raild-cli mount-railnfs mount-railfuse)
add_test(NAME e2e COMMAND rail-e2e)
