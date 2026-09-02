#include "gds-kernels.h"
#include "harness.h"
#include "kernel.h"
#include "local-process.h"
#include "privileged.h"

#include <cerrno>
#include <cstring>
#include <cuda_runtime.h>
#include <cufile.h>
#include <fcntl.h>
#include <fstream>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <vector>

namespace rail::e2e {

namespace {

constexpr const char *kNvidiaFsDevice = "/dev/nvidia-fs";
constexpr const char *kNvidiaFsStats = "/proc/driver/nvidia-fs/stats";
constexpr const char *kGdsSwitch = "/sys/module/railfs/parameters/gds";

std::string patternBytes(size_t Size, uint32_t Seed, uint64_t First = 0) {
  std::string Body(Size, '\0');
  for (size_t I = 0; I < Size; I++) {
    Body[I] = static_cast<char>(gdsPattern(First + I, Seed));
  }
  return Body;
}

bool ran(const std::vector<std::string> &Argv) {
  auto Ran = runLocal(Argv);
  return Ran && Ran->ExitStatus == 0;
}

std::string md5OnPeer(const std::string &Remote) {
  auto Ran = runLocal({"ssh", peerHost(), "md5sum", Remote});
  return Ran ? Ran->Output.substr(0, 32) : std::string{};
}

long nvidiaFsCount(const std::string &Counter) {
  const std::string Stats = readWholeFile(kNvidiaFsStats);
  const auto Line = Stats.find(Counter);
  if (Line == std::string::npos) return -1;
  const auto Count = Stats.find("n=", Line);
  if (Count == std::string::npos) return -1;
  return std::atol(Stats.c_str() + Count + 2);
}

bool loadNvidiaFs() {
  if (std::filesystem::exists(kNvidiaFsDevice)) return true;
  return ran({"modprobe", "nvidia_fs"}) && std::filesystem::exists(kNvidiaFsDevice);
}

std::string cufileMessage(CUfileError_t Status) { return cufileop_status_error(Status.err); }

class DeviceBuffer {
public:
  explicit DeviceBuffer(size_t Size) : Size(Size) {
    CUDA_CHECK(cudaMalloc(&Ptr, Size));
    Registered = cuFileBufRegister(Ptr, Size, 0).err == CU_FILE_SUCCESS;
  }

  ~DeviceBuffer() {
    if (Registered) cuFileBufDeregister(Ptr);
    cudaFree(Ptr);
  }

  DeviceBuffer(const DeviceBuffer &) = delete;
  DeviceBuffer &operator=(const DeviceBuffer &) = delete;

  bool ok() const { return Registered; }
  void *Ptr = nullptr;
  size_t Size;

private:
  bool Registered = false;
};

class GpuFile {
public:
  GpuFile(const std::string &Path, int Flags) {
    Fd = ::open(Path.c_str(), Flags | O_DIRECT | O_CLOEXEC, 0644);
    if (Fd < 0) {
      Error = std::string("open: ") + std::strerror(errno);
      return;
    }

    CUfileDescr_t Descriptor{};
    Descriptor.handle.fd = Fd;
    Descriptor.type = CU_FILE_HANDLE_TYPE_OPAQUE_FD;

    const auto Status = cuFileHandleRegister(&Handle, &Descriptor);
    if (Status.err != CU_FILE_SUCCESS) {
      Error = "cuFileHandleRegister: " + cufileMessage(Status);
      ::close(Fd);
      Fd = -1;
    }
  }

  ~GpuFile() { close(); }

  GpuFile(const GpuFile &) = delete;
  GpuFile &operator=(const GpuFile &) = delete;

  bool ok() const { return Fd >= 0; }

  void close() {
    if (Fd < 0) return;
    cuFileHandleDeregister(Handle);
    ::close(Fd);
    Fd = -1;
  }

  ssize_t read(void *Device, size_t Size, off_t FileOffset, off_t DeviceOffset) {
    return cuFileRead(Handle, Device, Size, FileOffset, DeviceOffset);
  }

  ssize_t write(const void *Device, size_t Size, off_t FileOffset, off_t DeviceOffset) {
    return cuFileWrite(Handle, Device, Size, FileOffset, DeviceOffset);
  }

  std::string Error;

private:
  int Fd = -1;
  CUfileHandle_t Handle{};
};

} // namespace

// GPUDirect Storage: cuFile moves bytes between GPU memory and the mount with
// the fabric writing into the GPU directly. Needs a GPU, nvidia-fs and the
// cuFile library on this side, so it is opt-in on top of the kernel suite.
class Gds : public Kernel {
protected:
  void SetUp() override {
    if (!::getenv("RAIL_GDS_TESTS")) GTEST_SKIP() << "set RAIL_GDS_TESTS=1 to run these";
    if (!loadNvidiaFs()) GTEST_SKIP() << "no " << kNvidiaFsDevice << " and modprobe nvidia_fs failed";

    Kernel::SetUp();
    if (IsSkipped() || HasFatalFailure()) return;

    CUDA_CHECK(cudaSetDevice(0));
    ASSERT_TRUE(clearKernelLog().has_value());

    const auto Opened = cuFileDriverOpen();
    ASSERT_EQ(Opened.err, CU_FILE_SUCCESS) << "cuFileDriverOpen: " << cufileMessage(Opened);
    DriverOpen = true;

    restartDaemonOnRdma();
    ASSERT_TRUE(mountIt(defaultOptions() + ",rdma,noverify"));
  }

  // nvidia-fs holds a reference on railfs from the moment it finds the
  // registration symbol, so railfs can only be unloaded once nvidia-fs is.
  void TearDown() override {
    if (DriverOpen) cuFileDriverClose();
    [[maybe_unused]] auto R = unloadModule("nvidia_fs");
    Kernel::TearDown();
  }

  void seedPattern(const std::string &Name, size_t Size, uint32_t Seed) {
    const auto Local = localDir() / Name;
    std::ofstream(Local, std::ios::binary | std::ios::trunc) << patternBytes(Size, Seed);
    seedRemote(Local, Export + "/" + Name);
  }

  bool DriverOpen = false;
};

TEST_F(Gds, RegistersWithNvidiaFs) {
  const std::string Log = kernelLog();
  EXPECT_NE(Log.find("nvidia-fs registered"), std::string::npos) << Log;
  EXPECT_EQ(readWholeFile(kGdsSwitch), "Y\n");
}

TEST_F(Gds, ReadsFromThePeerIntoGpuMemory) {
  constexpr size_t Size = (2u << 20) + 4096;
  constexpr uint32_t Seed = 11;
  seedPattern("gpu-read.bin", Size, Seed);

  DeviceBuffer Buffer(Size);
  ASSERT_TRUE(Buffer.ok());
  fillOnDevice(Buffer.Ptr, Size, Seed ^ 0xFFu, 0);

  const long Before = nvidiaFsCount("Reads");
  GpuFile File(Mountpoint + "/gpu-read.bin", O_RDONLY);
  ASSERT_TRUE(File.ok()) << File.Error;
  ASSERT_EQ(File.read(Buffer.Ptr, Size, 0, 0), static_cast<ssize_t>(Size)) << std::strerror(errno);

  EXPECT_EQ(mismatchesOnDevice(Buffer.Ptr, Size, Seed, 0), 0u) << "bytes in gpu memory differ from the file";

  // nvidia-fs counts only what went through it; a read that fell back to a
  // bounce buffer and cudaMemcpy would pass the bytes check and fail this.
  EXPECT_GT(nvidiaFsCount("Reads"), Before) << "nvidia-fs saw no read; cufile took the compat path";
}

TEST_F(Gds, ReadsARangeIntoTheMiddleOfABuffer) {
  constexpr size_t FileSize = 3u << 20;
  constexpr size_t Length = (1u << 20) + 4096;
  constexpr off_t FileOffset = 1u << 20;
  constexpr off_t DeviceOffset = 8192;
  constexpr uint32_t Seed = 12;
  seedPattern("gpu-range.bin", FileSize, Seed);

  DeviceBuffer Buffer(DeviceOffset + Length);
  ASSERT_TRUE(Buffer.ok());
  fillOnDevice(Buffer.Ptr, Buffer.Size, Seed ^ 0xFFu, 0);

  GpuFile File(Mountpoint + "/gpu-range.bin", O_RDONLY);
  ASSERT_TRUE(File.ok()) << File.Error;
  ASSERT_EQ(File.read(Buffer.Ptr, Length, FileOffset, DeviceOffset), static_cast<ssize_t>(Length)) << std::strerror(errno);

  const auto *Landed = static_cast<const uint8_t *>(Buffer.Ptr) + DeviceOffset;
  EXPECT_EQ(mismatchesOnDevice(Landed, Length, Seed, FileOffset), 0u) << "bytes landed wrong";
  EXPECT_EQ(mismatchesOnDevice(Buffer.Ptr, DeviceOffset, Seed ^ 0xFFu, 0), 0u) << "the read touched bytes before the device offset";
}

TEST_F(Gds, WritesFromGpuMemoryToThePeer) {
  constexpr size_t Size = (3u << 20) + 8192;
  constexpr uint32_t Seed = 13;
  seed("gpu-write.bin", "");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "666", Export + "/gpu-write.bin"}));

  DeviceBuffer Buffer(Size);
  ASSERT_TRUE(Buffer.ok());
  fillOnDevice(Buffer.Ptr, Size, Seed, 0);

  const long Before = nvidiaFsCount("Writes");
  {
    GpuFile File(Mountpoint + "/gpu-write.bin", O_WRONLY);
    ASSERT_TRUE(File.ok()) << File.Error;
    ASSERT_EQ(File.write(Buffer.Ptr, Size, 0, 0), static_cast<ssize_t>(Size)) << std::strerror(errno);
  }

  EXPECT_EQ(md5OnPeer(Export + "/gpu-write.bin"), digestBytes(patternBytes(Size, Seed))) << "the peer's copy differs from gpu memory";
  EXPECT_GT(nvidiaFsCount("Writes"), Before) << "nvidia-fs saw no write; cufile took the compat path";
}

TEST_F(Gds, WriteThenReadRoundTripsThroughTheFabric) {
  constexpr size_t Size = 4u << 20;
  constexpr uint32_t Seed = 14;
  seed("gpu-round.bin", "");
  ASSERT_TRUE(ran({"ssh", peerHost(), "chmod", "666", Export + "/gpu-round.bin"}));

  DeviceBuffer Out(Size);
  DeviceBuffer In(Size);
  ASSERT_TRUE(Out.ok());
  ASSERT_TRUE(In.ok());
  fillOnDevice(Out.Ptr, Size, Seed, 0);
  fillOnDevice(In.Ptr, Size, Seed ^ 0xFFu, 0);

  {
    GpuFile File(Mountpoint + "/gpu-round.bin", O_WRONLY);
    ASSERT_TRUE(File.ok()) << File.Error;
    ASSERT_EQ(File.write(Out.Ptr, Size, 0, 0), static_cast<ssize_t>(Size)) << std::strerror(errno);
  }

  ASSERT_TRUE(dropCaches().has_value());

  GpuFile File(Mountpoint + "/gpu-round.bin", O_RDONLY);
  ASSERT_TRUE(File.ok()) << File.Error;
  ASSERT_EQ(File.read(In.Ptr, Size, 0, 0), static_cast<ssize_t>(Size)) << std::strerror(errno);

  EXPECT_EQ(mismatchesOnDevice(In.Ptr, Size, Seed, 0), 0u) << "bytes changed on the way there and back";
}

} // namespace rail::e2e
