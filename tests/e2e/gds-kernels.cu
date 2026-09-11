#include "gds-kernels.h"

namespace rail::e2e {

namespace {

constexpr unsigned kBlocks = 1024;
constexpr unsigned kThreads = 256;

__device__ size_t globalIndex() { return static_cast<size_t>(blockIdx.x) * blockDim.x + threadIdx.x; }
__device__ size_t gridStride() { return static_cast<size_t>(gridDim.x) * blockDim.x; }

__global__ void fillKernel(uint8_t *Buffer, size_t Size, uint32_t Seed, uint64_t First) {
  const size_t Stride = gridStride();
  for (size_t I = globalIndex(); I < Size; I += Stride) Buffer[I] = gdsPattern(First + I, Seed);
}

__global__ void mismatchKernel(const uint8_t *Buffer, size_t Size, uint32_t Seed, uint64_t First, unsigned long long *Mismatches) {
  const size_t Stride = gridStride();
  unsigned long long Mine = 0;
  for (size_t I = globalIndex(); I < Size; I += Stride) Mine += Buffer[I] != gdsPattern(First + I, Seed);
  if (Mine) atomicAdd(Mismatches, Mine);
}

cudaLaunchConfig_t wholeBuffer() {
  cudaLaunchConfig_t Config = {};
  Config.gridDim = dim3(kBlocks);
  Config.blockDim = dim3(kThreads);
  return Config;
}

} // namespace

void fillOnDevice(void *Buffer, size_t Size, uint32_t Seed, uint64_t First) {
  const cudaLaunchConfig_t Config = wholeBuffer();
  LAUNCH_KERNEL(&Config, fillKernel, static_cast<uint8_t *>(Buffer), Size, Seed, First);
  CUDA_CHECK(cudaDeviceSynchronize());
}

uint64_t mismatchesOnDevice(const void *Buffer, size_t Size, uint32_t Seed, uint64_t First) {
  const cudaLaunchConfig_t Config = wholeBuffer();
  unsigned long long *Count = nullptr;
  unsigned long long Found = 0;

  CUDA_CHECK(cudaMalloc(&Count, sizeof(*Count)));
  CUDA_CHECK(cudaMemset(Count, 0, sizeof(*Count)));
  LAUNCH_KERNEL(&Config, mismatchKernel, static_cast<const uint8_t *>(Buffer), Size, Seed, First, Count);
  CUDA_CHECK(cudaDeviceSynchronize());
  CUDA_CHECK(cudaMemcpy(&Found, Count, sizeof(Found), cudaMemcpyDeviceToHost));
  CUDA_CHECK(cudaFree(Count));
  return Found;
}

} // namespace rail::e2e
