find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBSSH REQUIRED IMPORTED_TARGET libssh)

# Not part of the library: this is what the end-to-end tests and the benchmarks
# use to drive a peer and a local process, so neither has to build shell
# command lines to do it.
add_library(rail-testing STATIC
  remote-host.cc
  local-process.cc
)
target_include_directories(rail-testing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(rail-testing PUBLIC rail PkgConfig::LIBSSH)
