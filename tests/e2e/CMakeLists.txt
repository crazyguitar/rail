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

# The GPUDirect Storage suite needs nvcc and cuFile, so it joins the binary
# only where a CUDA toolkit is found and is skipped everywhere else.
include(CheckLanguage)
check_language(CUDA)
if(CMAKE_CUDA_COMPILER)
  if(NOT DEFINED CMAKE_CUDA_ARCHITECTURES)
    set(CMAKE_CUDA_ARCHITECTURES 70 80 90)
  endif()
  enable_language(CUDA)
  find_package(CUDAToolkit)
endif()

if(TARGET CUDA::cuFile)
  target_sources(rail-e2e PRIVATE test-gds.cc gds-kernels.cu)
  target_link_libraries(rail-e2e PRIVATE CUDA::cudart CUDA::cuFile)
  message(STATUS "rail-e2e: GPUDirect Storage suite on")
else()
  message(STATUS "rail-e2e: GPUDirect Storage suite off; no CUDA toolkit with cuFile")
endif()
