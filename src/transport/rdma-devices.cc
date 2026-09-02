#include "rail/transport/rdma-devices.h"

#include <format>

#ifdef RAIL_HAVE_RDMA
#include <infiniband/verbs.h>
#endif

namespace rail {

#ifndef RAIL_HAVE_RDMA

// Without an RDMA transport there is nothing to enumerate, and verbs may not
// even be installed. Callers get an empty list and need no build-time guard.
std::vector<RdmaPort> activeRdmaPorts() { return {}; }

#else

std::vector<RdmaPort> activeRdmaPorts() {
  std::vector<RdmaPort> Active;

  int Count = 0;
  ibv_device **List = ibv_get_device_list(&Count);
  if (!List) return Active;

  for (int I = 0; I < Count; I++) {
    ibv_context *Ctx = ibv_open_device(List[I]);
    if (!Ctx) continue;

    ibv_device_attr Dev{};
    if (ibv_query_device(Ctx, &Dev) == 0) {
      for (uint8_t P = 1; P <= Dev.phys_port_cnt; P++) {
        ibv_port_attr Port{};
        if (ibv_query_port(Ctx, P, &Port) != 0) continue;
        if (Port.state != IBV_PORT_ACTIVE) continue;
        Active.push_back({ibv_get_device_name(List[I]), P});
      }
    }
    ibv_close_device(Ctx);
  }

  ibv_free_device_list(List);
  return Active;
}

#endif

std::string describe(const std::vector<RdmaPort> &Ports) {
  std::string Text;
  for (const RdmaPort &P : Ports) {
    if (!Text.empty()) Text += ",";
    Text += std::format("{}:{}", P.Device, P.Port);
  }
  return Text;
}

} // namespace rail
