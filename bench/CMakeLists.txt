set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(benchmark
  GIT_REPOSITORY https://github.com/google/benchmark.git
  GIT_TAG v1.9.1
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(benchmark)

add_executable(rail-bench
  bench-p2p.cc
  bench-rail.cc
  bench-nfs.cc
  bench-fuse.cc
  bench-railfs.cc
  bench-compare.cc
)
target_link_libraries(rail-bench PRIVATE rail-testing benchmark::benchmark)
add_dependencies(rail-bench rail-cli raild-cli mount-railnfs mount-railfuse)
