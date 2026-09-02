cmake_minimum_required(VERSION 3.28)
project(rail C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake)

# Takes a sanitizer list verbatim: address,undefined or thread.
set(RAIL_SANITIZE "" CACHE STRING "Build with a sanitizer: address,undefined or thread")

option(RAIL_ENABLE_RDMA "Build the native verbs data channel" ON)
option(RAIL_BUILD_TESTS "Build end-to-end tests" ON)
option(RAIL_BUILD_BENCH "Build the RDMA benchmarks" ON)

if(RAIL_SANITIZE)
  add_compile_options(-fsanitize=${RAIL_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${RAIL_SANITIZE})
endif()

include(FetchContent)

set(XXHASH_BUILD_XXHSUM OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(xxhash
  GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
  GIT_TAG v0.8.3
  SOURCE_SUBDIR cmake_unofficial
  EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(xxhash)

add_library(rail)
target_include_directories(rail PUBLIC include)

find_package(PkgConfig REQUIRED)
pkg_check_modules(URING REQUIRED IMPORTED_TARGET liburing)
pkg_check_modules(FUSE3 REQUIRED IMPORTED_TARGET fuse3)
target_link_libraries(rail PUBLIC xxHash::xxhash PkgConfig::URING PkgConfig::FUSE3)

if(RAIL_ENABLE_RDMA)
  pkg_check_modules(IBVERBS REQUIRED IMPORTED_TARGET libibverbs)
  target_link_libraries(rail PUBLIC PkgConfig::IBVERBS)
  target_compile_definitions(rail PUBLIC RAIL_HAVE_RDMA=1)
endif()

add_subdirectory(src)
add_subdirectory(tools/rail)
add_subdirectory(tools/raild)
add_subdirectory(tools/mount.railfuse)
add_subdirectory(tools/mount.railfs)
add_subdirectory(tools/mount.railnfs)

# The kernel module, so one build produces every artefact this project has.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_subdirectory(src/linux)
endif()

if(RAIL_BUILD_TESTS OR RAIL_BUILD_BENCH)
  add_subdirectory(src/testing)
endif()

# The benchmarks drive the fabric directly, so they need the verbs backend.
if(RAIL_BUILD_BENCH AND RAIL_ENABLE_RDMA)
  add_subdirectory(bench)
endif()

if(RAIL_BUILD_TESTS)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
  add_subdirectory(tests/e2e)
endif()
