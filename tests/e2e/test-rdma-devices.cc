#include "../../src/linux/rdma-link-rate.h"
#include "rail/transport/rdma-devices.h"

#include <fstream>
#include <gtest/gtest.h>

namespace rail::e2e {

TEST(RdmaLinkRate, DecodesLaneWidthsAndSpeeds) {
  EXPECT_EQ(rail_rdma_rate_mbps(32, 2), 100000u);
  EXPECT_EQ(rail_rdma_rate_mbps(64, 2), 200000u);
  EXPECT_EQ(rail_rdma_rate_mbps(64, 16), 100000u);
  EXPECT_EQ(rail_rdma_rate_mbps(4, 8), 120000u);
  EXPECT_EQ(rail_rdma_rate_mbps(16, 2), 56000u);
  EXPECT_EQ(rail_rdma_rate_mbps(128, 2), 400000u);
}

TEST(RdmaLinkRate, UnknownEncodingsHaveNoKnownRate) {
  EXPECT_EQ(rail_rdma_rate_mbps(0, 2), 0u);
  EXPECT_EQ(rail_rdma_rate_mbps(64, 0), 0u);
}

TEST(RdmaPorts, FasterPortsPrecedeManagementPorts) {
  const auto Ports = activeRdmaPorts();
  if (Ports.empty()) GTEST_SKIP() << "no active RDMA ports";

  double PreviousRate = 0;
  std::string PreviousName;
  uint8_t PreviousPort = 0;
  for (const auto &Port : Ports) {
    const auto Path = "/sys/class/infiniband/" + Port.Device + "/ports/" + std::to_string(Port.Port) + "/rate";
    std::ifstream RateFile(Path);
    double Rate = 0;
    ASSERT_TRUE(static_cast<bool>(RateFile >> Rate)) << Path;
    EXPECT_EQ(Port.RateMbps, static_cast<uint32_t>(Rate * 1000)) << Port.Device;
    if (!PreviousName.empty()) EXPECT_GE(PreviousRate, Rate) << PreviousName << " precedes faster " << Port.Device;
    if (PreviousRate == Rate) {
      EXPECT_LE(PreviousName, Port.Device);
      if (PreviousName == Port.Device) EXPECT_LT(PreviousPort, Port.Port);
    }
    PreviousRate = Rate;
    PreviousName = Port.Device;
    PreviousPort = Port.Port;
  }
}

} // namespace rail::e2e
