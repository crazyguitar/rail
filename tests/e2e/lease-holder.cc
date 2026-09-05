#include <chrono>
#include <csignal>
#include <cstdio>
#include <fcntl.h>
#include <thread>
#include <unistd.h>

// Holds a read lease until the test answers an unrelated request and creates
// the release marker. Timeouts bound failures without making latency an
// assertion: the test checks which event released the lease.
int main(int Argc, char **Argv) {
  if (Argc != 3) return 2;
  ::alarm(20);
  sigset_t Signals;
  ::sigemptyset(&Signals);
  ::sigaddset(&Signals, SIGIO);
  if (::sigprocmask(SIG_BLOCK, &Signals, nullptr) != 0) return 1;

  const int Fd = ::open(Argv[1], O_RDONLY | O_CLOEXEC);
  if (Fd < 0 || ::fcntl(Fd, F_SETLEASE, F_RDLCK) < 0) {
    std::perror("read lease");
    return 1;
  }
  std::puts("ready");
  std::fflush(stdout);

  const ::timespec Patience{10, 0};
  if (::sigtimedwait(&Signals, nullptr, &Patience) != SIGIO) return 1;
  std::puts("blocked");
  std::fflush(stdout);

  const auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool Released = false;
  while (!(Released = ::access(Argv[2], F_OK) == 0) && std::chrono::steady_clock::now() < Deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

  if (::fcntl(Fd, F_SETLEASE, F_UNLCK) < 0) return 1;
  ::close(Fd);
  std::puts(Released ? "released" : "timed out");
  return Released ? 0 : 1;
}
