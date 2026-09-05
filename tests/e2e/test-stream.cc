#include "rail/fs/reader.h"
#include "rail/io/runner.h"
#include "rail/page-pool.h"
#include "rail/stream/page-stream.h"
#include "rail/stream/sink.h"
#include "rail/transport/data-channel.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <vector>

using namespace rail;

namespace {

// A channel whose send works FailAfter times, then fails every one:
//
//   send #1 ok   send #2 ok   send #3 fail   send #4 fail ...
//
// so a stream can be pushed into an early return with pages still queued.
// Only send is real; nothing here receives.
class FlakyChannel final : public DataChannel {
public:
  FlakyChannel(size_t PageCount, size_t PageSize, size_t FailAfter) : Pool(PageCount, PageSize), FailAfter(FailAfter) {}

  DataChannelTraits traits() const override { return {false, 1u << 22, 16, "flaky"}; }
  PagePool &pool() override { return Pool; }
  Coro<Result<std::string>> listen() override { co_return std::string{}; }
  Coro<Result<void>> connect(const std::string &) override { co_return Result<void>{}; }
  Coro<Result<void>> acceptPeer() override { co_return Result<void>{}; }
  Coro<Result<void>> recv(Page &, uint64_t, size_t) override { co_return failMessage("flaky channel does not receive"); }

  Coro<Result<void>> send(Page &, uint64_t) override {
    if (Sent++ >= FailAfter) co_return failMessage("flaky channel refused a send");
    co_return Result<void>{};
  }

  void close() override {}

private:
  PagePool Pool;
  size_t FailAfter;
  size_t Sent = 0;
};

std::filesystem::path writeTemp(const std::string &Name, size_t Bytes) {
  const auto Path = std::filesystem::temp_directory_path() / Name;
  std::ofstream Out(Path, std::ios::binary | std::ios::trunc);
  const std::vector<char> Block(Bytes, 'x');
  Out.write(Block.data(), static_cast<std::streamsize>(Block.size()));
  return Path;
}

} // namespace

// Some pages are marked failed, then a send fails partway, so run() returns
// early with failed pages still queued and quiesce has to clean them up:
//
//   fill:  [p0][p1 fail][p2 fail] ...     send: p0 ok, p1 send fails
//   run() returns early  ->  queue still holds p1,p2 (read never submitted)
//   quiesce: await(p1.read)  -->  hangs unless the read was marked done
//
// stream() must return rather than hang.
TEST(Stream, QuiesceReturnsWhenASendFailsWithUnreadPagesQueued) {
  constexpr size_t kPage = 64u << 10;
  constexpr size_t kPages = 8;
  const auto Src = writeTemp("rail-stream-quiesce.bin", kPage * kPages);

  auto Reader = FileReader::open(Src);
  ASSERT_TRUE(Reader) << Reader.error().message();
  FileSource Source(*Reader);

  FlakyChannel Channel(kPages, kPage, /*FailAfter=*/1);
  const StreamGeometry Geometry = StreamGeometry::forChannel(Channel);

  PageSender Sender(Channel, Source, 0, Geometry, false, true, Sum::XxH3, /*AbortAfterPages=*/1);
  const auto Out = run(Sender.stream(0, kPage * kPages));
  EXPECT_FALSE(Out) << "a stream that lost a send should fail, not succeed";

  std::filesystem::remove(Src);
}
