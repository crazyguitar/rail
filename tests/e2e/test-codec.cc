#include "rail/proto/codec.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <vector>

using namespace rail;

namespace {

std::vector<std::byte> listReplyClaiming(uint32_t Count) {
  std::vector<std::byte> Payload;
  proto::Writer W(Payload);
  W.u64(7);
  W.u8(1);
  W.u32(Count);
  return Payload;
}

} // namespace

TEST(Codec, AListReplyClaimingMoreEntriesThanItCarriesIsRefusedAtOnce) {
  const auto Payload = listReplyClaiming(0xffffffffu);

  const auto Began = std::chrono::steady_clock::now();
  auto M = proto::decode(proto::Type::ListReply, Payload);
  const auto Took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Began).count();

  EXPECT_FALSE(M.has_value()) << "a count the payload cannot hold was accepted";
  EXPECT_LT(Took, 100) << "refusing the count took " << Took << " ms";
}

TEST(Codec, AListReplyRoundTrips) {
  proto::ListReply Reply;
  Reply.Id = 3;
  Reply.Found = true;
  Reply.Entries.push_back({"alpha", {}});
  Reply.Entries.push_back({"beta", {}});
  Reply.Entries.back().Attrs.Size = 42;

  std::vector<std::byte> Payload;
  proto::encode(proto::Message{Reply}, Payload);

  auto M = proto::decode(proto::Type::ListReply, Payload);
  ASSERT_TRUE(M.has_value()) << M.error().message();
  auto *Back = std::get_if<proto::ListReply>(&*M);
  ASSERT_NE(Back, nullptr);
  EXPECT_EQ(Back->Id, 3u);
  EXPECT_TRUE(Back->Found);
  ASSERT_EQ(Back->Entries.size(), 2u);
  EXPECT_EQ(Back->Entries[0].Name, "alpha");
  EXPECT_EQ(Back->Entries[1].Name, "beta");
  EXPECT_EQ(Back->Entries[1].Attrs.Size, 42u);
}

TEST(Codec, AListReplyCutShortIsRefused) {
  proto::ListReply Reply;
  Reply.Id = 3;
  Reply.Found = true;
  Reply.Entries.push_back({"alpha", {}});
  Reply.Entries.push_back({"beta", {}});

  std::vector<std::byte> Payload;
  proto::encode(proto::Message{Reply}, Payload);
  Payload.resize(Payload.size() - 3);

  EXPECT_FALSE(proto::decode(proto::Type::ListReply, Payload).has_value());
}
