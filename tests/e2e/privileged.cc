#include "privileged.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/statfs.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <sys/stat.h>
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <cstdlib>
#include <vector>

namespace rail::e2e {
namespace {

Result<void> closing(int Fd, int Result) {
  const int Kept = errno;
  ::close(Fd);
  if (Result != 0) {
    errno = Kept;
    return failErrno("module");
  }

  return {};
}

} // namespace

bool runningAsRoot() { return ::geteuid() == 0; }

bool waitForListener(const std::string &Host, uint16_t Port, std::chrono::milliseconds Patience) {
  const auto Deadline = std::chrono::steady_clock::now() + Patience;

  sockaddr_in Where{};
  Where.sin_family = AF_INET;
  Where.sin_port = ::htons(Port);
  if (::inet_pton(AF_INET, Host.c_str(), &Where.sin_addr) != 1) return false;

  while (std::chrono::steady_clock::now() < Deadline) {
    const int Sock = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (Sock < 0) return false;

    const bool Up = ::connect(Sock, (sockaddr *)&Where, sizeof(Where)) == 0;
    ::close(Sock);
    if (Up) return true;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  return false;
}

Result<void> mountFilesystem(const std::string &Type, const std::string &Source, const std::string &Target, const std::string &Options) {
  if (::mount(Source.c_str(), Target.c_str(), Type.c_str(), 0, Options.c_str()) != 0) return failErrno("mount " + Target);

  return {};
}

Result<void> unmountFilesystem(const std::string &Target, bool Lazy) {
  if (::umount2(Target.c_str(), Lazy ? MNT_DETACH : 0) != 0) return failErrno("umount " + Target);

  return {};
}

Result<void> loadModule(const std::filesystem::path &Object, const std::string &Parameters) {
  const int Fd = ::open(Object.c_str(), O_RDONLY | O_CLOEXEC);
  if (Fd < 0) return failErrno("open " + Object.string());

  return closing(Fd, (int)::syscall(SYS_finit_module, Fd, Parameters.c_str(), 0));
}

Result<void> unloadModule(const std::string &Name) {
  if (::syscall(SYS_delete_module, Name.c_str(), O_NONBLOCK) != 0) return failErrno("rmmod " + Name);

  return {};
}

Result<void> dropCaches() {
  ::sync();

  const int Fd = ::open("/proc/sys/vm/drop_caches", O_WRONLY | O_CLOEXEC);
  if (Fd < 0) return failErrno("open drop_caches");

  const ssize_t Wrote = ::write(Fd, "3\n", 2);
  ::close(Fd);
  if (Wrote != 2) return failErrno("write drop_caches");

  return {};
}

bool createEmpty(const std::filesystem::path &At) {
  const int Fd = ::open(At.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
  if (Fd < 0) return false;

  ::close(Fd);
  return true;
}

bool renameTo(const std::filesystem::path &From, const std::filesystem::path &To) { return ::rename(From.c_str(), To.c_str()) == 0; }

bool resizeTo(const std::filesystem::path &At, uint64_t Length) { return ::truncate(At.c_str(), (off_t)Length) == 0; }

bool setMode(const std::filesystem::path &At, unsigned int Mode) { return ::chmod(At.c_str(), (mode_t)Mode) == 0; }

bool setModifiedTime(const std::filesystem::path &At, int64_t Seconds) {
  const struct timespec When[2] = {{(time_t)Seconds, 0}, {(time_t)Seconds, 0}};
  return ::utimensat(AT_FDCWD, At.c_str(), When, 0) == 0;
}

bool makeSymlink(const std::string &Target, const std::filesystem::path &At) { return ::symlink(Target.c_str(), At.c_str()) == 0; }

bool makeHardLink(const std::filesystem::path &Existing, const std::filesystem::path &At) { return ::link(Existing.c_str(), At.c_str()) == 0; }

bool syncFilesystem(const std::filesystem::path &At) {
  const int Fd = ::open(At.c_str(), O_RDONLY | O_CLOEXEC);
  if (Fd < 0) return false;

  const int Synced = ::syncfs(Fd);
  ::close(Fd);
  return Synced == 0;
}

bool copyInto(const std::filesystem::path &From, const std::filesystem::path &To, bool Direct) {
  const int In = ::open(From.c_str(), O_RDONLY | O_CLOEXEC);
  if (In < 0) return false;

  const int Out = ::open(To.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (Direct ? O_DIRECT : 0), 0644);
  if (Out < 0) {
    ::close(In);
    return false;
  }

  // O_DIRECT needs the buffer, the offset and the length all aligned, so the
  // copy runs in whole blocks and the tail goes back through the page cache.
  constexpr size_t Block = 1u << 20;
  void *Buffer = nullptr;
  bool Ok = ::posix_memalign(&Buffer, 4096, Block) == 0;

  uint64_t Copied = 0;
  while (Ok) {
    const ssize_t Got = ::pread(In, Buffer, Block, (off_t)Copied);
    if (Got <= 0) break;

    const size_t Whole = Direct ? ((size_t)Got / 4096) * 4096 : (size_t)Got;
    if (Whole && ::pwrite(Out, Buffer, Whole, (off_t)Copied) != (ssize_t)Whole) Ok = false;
    Copied += Whole;

    if ((size_t)Got != Whole) {
      const int Tail = ::open(To.c_str(), O_WRONLY | O_CLOEXEC);
      Ok = Tail >= 0 && ::pwrite(Tail, (char *)Buffer + Whole, (size_t)Got - Whole, (off_t)Copied) == (ssize_t)((size_t)Got - Whole);
      if (Tail >= 0) ::close(Tail);
      Copied += (size_t)Got - Whole;
      break;
    }
  }

  ::free(Buffer);
  Ok = Ok && ::fsync(Out) == 0;
  ::close(Out);
  ::close(In);
  return Ok;
}

std::string readWholeFile(const std::filesystem::path &At) {
  const int Fd = ::open(At.c_str(), O_RDONLY | O_CLOEXEC);
  if (Fd < 0) return {};

  std::string Body;
  char Chunk[4096];
  for (ssize_t Got = 0; (Got = ::read(Fd, Chunk, sizeof(Chunk))) > 0;) Body.append(Chunk, (size_t)Got);

  ::close(Fd);
  return Body;
}

// Truncating, so a shorter body leaves no tail of the last one behind.
bool writeWholeFile(const std::filesystem::path &At, const std::string &Body) {
  const int Fd = ::open(At.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (Fd < 0) return false;

  const ssize_t Wrote = ::write(Fd, Body.data(), Body.size());
  ::close(Fd);
  return Wrote == (ssize_t)Body.size();
}

namespace {

class Md5 {
public:
  void update(const unsigned char *Data, size_t Count) {
    Length += Count;

    while (Count > 0) {
      const size_t Take = std::min(sizeof(Block) - Filled, Count);
      std::memcpy(Block + Filled, Data, Take);
      Filled += Take;
      Data += Take;
      Count -= Take;

      if (Filled == sizeof(Block)) {
        transform(Block);
        Filled = 0;
      }
    }
  }

  std::string finish() {
    const uint64_t Bits = Length * 8;
    unsigned char Pad = 0x80;
    update(&Pad, 1);

    Pad = 0;
    while (Filled != 56) {
      update(&Pad, 1);
    }

    unsigned char Tail[8];
    for (int I = 0; I < 8; ++I) {
      Tail[I] = (unsigned char)(Bits >> (8 * I));
    }

    update(Tail, 8);

    const uint32_t Words[4] = {A, B, C, D};
    std::string Hex;
    for (uint32_t Word : Words) {
      for (int I = 0; I < 4; ++I) {
        char Pair[3];
        std::snprintf(Pair, sizeof(Pair), "%02x", (unsigned)((Word >> (8 * I)) & 0xff));
        Hex += Pair;
      }
    }

    return Hex;
  }

private:
  void transform(const unsigned char *P) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                              5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                              4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    uint32_t M[16];
    for (int I = 0; I < 16; ++I) {
      M[I] = (uint32_t)P[I * 4] | ((uint32_t)P[I * 4 + 1] << 8) | ((uint32_t)P[I * 4 + 2] << 16) |
             ((uint32_t)P[I * 4 + 3] << 24);
    }

    uint32_t W = A, X = B, Y = C, Z = D;
    for (int I = 0; I < 64; ++I) {
      uint32_t F;
      int G;

      if (I < 16) {
        F = (X & Y) | (~X & Z);
        G = I;
      } else if (I < 32) {
        F = (Z & X) | (~Z & Y);
        G = (5 * I + 1) % 16;
      } else if (I < 48) {
        F = X ^ Y ^ Z;
        G = (3 * I + 5) % 16;
      } else {
        F = Y ^ (X | ~Z);
        G = (7 * I) % 16;
      }

      F += W + K[I] + M[G];
      W = Z;
      Z = Y;
      Y = X;
      X += (F << S[I]) | (F >> (32 - S[I]));
    }

    A += W;
    B += X;
    C += Y;
    D += Z;
  }

  uint32_t A = 0x67452301, B = 0xefcdab89, C = 0x98badcfe, D = 0x10325476;
  uint64_t Length = 0;
  unsigned char Block[64]{};
  size_t Filled = 0;
};

void *alignedBuffer(size_t Bytes) {
  void *Buffer = nullptr;
  if (::posix_memalign(&Buffer, 4096, Bytes) != 0) return nullptr;
  return Buffer;
}

} // namespace

std::string digestBytes(const std::string &Body) {
  Md5 Hash;
  Hash.update((const unsigned char *)Body.data(), Body.size());
  return Hash.finish();
}

std::string digestFile(const std::filesystem::path &At) {
  const int Fd = ::open(At.c_str(), O_RDONLY | O_CLOEXEC);
  if (Fd < 0) return {};

  Md5 Hash;
  std::string Chunk(1u << 20, '\0');
  for (;;) {
    const ssize_t Got = ::read(Fd, Chunk.data(), Chunk.size());
    if (Got <= 0) break;
    Hash.update((const unsigned char *)Chunk.data(), (size_t)Got);
  }

  ::close(Fd);
  return Hash.finish();
}

std::string digestMapped(const std::filesystem::path &At) {
  const int Fd = ::open(At.c_str(), O_RDONLY | O_CLOEXEC);
  if (Fd < 0) return {};

  std::string Hex;
  struct stat Info{};
  if (::fstat(Fd, &Info) == 0 && Info.st_size > 0) {
    void *Where = ::mmap(nullptr, (size_t)Info.st_size, PROT_READ, MAP_SHARED, Fd, 0);
    if (Where != MAP_FAILED) {
      Md5 Hash;
      Hash.update((const unsigned char *)Where, (size_t)Info.st_size);
      Hex = Hash.finish();
      ::munmap(Where, (size_t)Info.st_size);
    }
  }

  ::close(Fd);
  return Hex;
}

std::string digestDirect(const std::filesystem::path &At, size_t Block, uint64_t SkipBlocks) {
  const int Fd = ::open(At.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC);
  if (Fd < 0) return {};

  const size_t Room = ((Block + 4095) / 4096) * 4096;
  void *Buffer = alignedBuffer(Room);
  if (!Buffer) {
    ::close(Fd);
    return {};
  }

  std::string Hex;
  if (::lseek(Fd, (off_t)(SkipBlocks * Block), SEEK_SET) >= 0) {
    Md5 Hash;
    for (;;) {
      const ssize_t Got = ::read(Fd, Buffer, Room);
      if (Got <= 0) break;
      Hash.update((const unsigned char *)Buffer, (size_t)Got);
    }
    Hex = Hash.finish();
  }

  ::free(Buffer);
  ::close(Fd);
  return Hex;
}

std::string readRange(const std::filesystem::path &At, uint64_t Offset, uint64_t Length) {
  const int Fd = ::open(At.c_str(), O_RDONLY | O_CLOEXEC);
  if (Fd < 0) return {};

  std::string Body(Length, '\0');
  const ssize_t Got = ::pread(Fd, Body.data(), Length, (off_t)Offset);
  ::close(Fd);

  if (Got < 0) return {};

  Body.resize((size_t)Got);
  return Body;
}

std::vector<std::string> listNames(const std::filesystem::path &At) {
  std::vector<std::string> Names;
  DIR *Directory = ::opendir(At.c_str());
  if (!Directory) return Names;

  while (const dirent *Entry = ::readdir(Directory)) {
    const std::string Name = Entry->d_name;
    if (Name != "." && Name != "..") Names.push_back(Name);
  }

  ::closedir(Directory);
  return Names;
}

uint64_t inodeFromListing(const std::filesystem::path &Directory, const std::string &Name) {
  DIR *Open = ::opendir(Directory.c_str());
  if (!Open) return 0;

  uint64_t Number = 0;
  while (const dirent *Entry = ::readdir(Open)) {
    if (Name == Entry->d_name) {
      Number = Entry->d_ino;
      break;
    }
  }

  ::closedir(Open);
  return Number;
}

std::vector<std::string> filesUnder(const std::filesystem::path &At) {
  std::vector<std::string> Found;
  std::error_code Failed;

  for (auto It = std::filesystem::recursive_directory_iterator(At, Failed);
       !Failed && It != std::filesystem::recursive_directory_iterator(); It.increment(Failed)) {
    if (It->is_regular_file(Failed)) Found.push_back(It->path().string());
  }

  return Found;
}

bool isMountpoint(const std::filesystem::path &At) {
  struct stat Here{};
  struct stat Above{};

  if (::stat(At.c_str(), &Here) != 0) return false;
  if (::stat((At / "..").c_str(), &Above) != 0) return false;

  return Here.st_dev != Above.st_dev || Here.st_ino == Above.st_ino;
}

int64_t fileSize(const std::filesystem::path &At) {
  struct stat Info{};
  if (::stat(At.c_str(), &Info) != 0) return -1;
  return (int64_t)Info.st_size;
}

uint64_t inodeOf(const std::filesystem::path &At) {
  struct stat Info{};
  if (::stat(At.c_str(), &Info) != 0) return 0;
  return (uint64_t)Info.st_ino;
}

int64_t modifiedTime(const std::filesystem::path &At) {
  struct stat Info{};
  if (::stat(At.c_str(), &Info) != 0) return -1;
  return (int64_t)Info.st_mtime;
}

uint64_t freeBlocks(const std::filesystem::path &At) {
  struct statfs Info{};
  if (::statfs(At.c_str(), &Info) != 0) return 0;
  return (uint64_t)Info.f_blocks;
}

std::string linkTarget(const std::filesystem::path &At) {
  char Buffer[4096];
  const ssize_t Got = ::readlink(At.c_str(), Buffer, sizeof(Buffer));
  if (Got <= 0) return {};
  return std::string(Buffer, (size_t)Got);
}

Result<void> clearKernelLog() {
  // SYSLOG_ACTION_CLEAR.
  if (::klogctl(5, nullptr, 0) < 0) return failErrno("clear the kernel log");

  return {};
}

Result<std::string> readKernelLog() {
  // SYSLOG_ACTION_SIZE_BUFFER, then SYSLOG_ACTION_READ_ALL.
  const int Size = ::klogctl(10, nullptr, 0);
  if (Size < 0) return failErrno("size the kernel log");

  std::vector<char> Buffer((size_t)Size + 1);
  const int Read = ::klogctl(3, Buffer.data(), Size);
  if (Read < 0) return failErrno("read the kernel log");

  return std::string(Buffer.data(), (size_t)Read);
}

} // namespace rail::e2e
