#pragma once

#include <cstddef>
#include <cstdint>
#include <cuda_runtime.h>
#include <stdexcept>
#include <string>

#ifdef __CUDACC__
#define RAIL_HOST_DEVICE __host__ __device__
#else
#define RAIL_HOST_DEVICE
#endif

#define CUDA_CHECK(exp)                                                                                                                \
  do {                                                                                                                                 \
    const cudaError_t Err = (exp);                                                                                                     \
    if (Err != cudaSuccess) {                                                                                                          \
      throw std::runtime_error(std::string(__FILE__ ":") + std::to_string(__LINE__) + " " #exp " failed: " + cudaGetErrorString(Err)); \
    }                                                                                                                                  \
  } while (0)

#define LAUNCH_KERNEL(cfg, kernel, ...) CUDA_CHECK(cudaLaunchKernelEx(cfg, kernel, ##__VA_ARGS__))

namespace rail::e2e {

RAIL_HOST_DEVICE inline uint8_t gdsPattern(uint64_t Index, uint32_t Seed) {
  uint32_t X = static_cast<uint32_t>(Index) * 0x9E3779B1u ^ (Seed * 0x85EBCA77u);
  X ^= X >> 15;
  X *= 0x2C1B3C6Du;
  X ^= X >> 12;
  return static_cast<uint8_t>(X >> 24);
}

void fillOnDevice(void *Buffer, size_t Size, uint32_t Seed, uint64_t First);
uint64_t mismatchesOnDevice(const void *Buffer, size_t Size, uint32_t Seed, uint64_t First);

} // namespace rail::e2e
