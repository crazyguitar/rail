#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rail {

struct RdmaPort {
  std::string Device;
  uint8_t Port = 1;
  uint32_t RateMbps = 0;
};

// Fastest first, then device name and port number.
std::vector<RdmaPort> activeRdmaPorts();

std::string describe(const std::vector<RdmaPort> &Ports);

} // namespace rail
