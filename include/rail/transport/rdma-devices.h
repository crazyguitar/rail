#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace rail {

struct RdmaPort {
  std::string Device;
  uint8_t Port = 1;
};

// Ports in the ACTIVE state. UCX stripes a rendezvous transfer across these,
// so an empty list means it will quietly fall back to sockets over whatever
// ordinary network exists, at a fraction of the speed.
std::vector<RdmaPort> activeRdmaPorts();

std::string describe(const std::vector<RdmaPort> &Ports);

} // namespace rail
