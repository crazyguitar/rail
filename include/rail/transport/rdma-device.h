#pragma once

#include "rail/result.h"

#include <infiniband/verbs.h>

#include <memory>
#include <string>

namespace rail {

class RdmaDevice {
public:
  ~RdmaDevice();

  RdmaDevice(const RdmaDevice &) = delete;
  RdmaDevice &operator=(const RdmaDevice &) = delete;

  static Result<std::shared_ptr<RdmaDevice>> open(const std::string &Name);

  ibv_context *context() const { return Context; }
  ibv_pd *domain() const { return Domain; }
  const std::string &name() const { return Name; }

  // Index into a region's per-device registration table. Stable for the life
  // of the process, which is what lets a page find its key by lookup rather
  // than by search.
  size_t slot() const { return Slot; }

private:
  RdmaDevice(std::string Name, ibv_context *Context, ibv_pd *Domain, size_t Slot)
      : Name(std::move(Name)), Context(Context), Domain(Domain), Slot(Slot) {}

  std::string Name;
  ibv_context *Context = nullptr;
  ibv_pd *Domain = nullptr;
  size_t Slot = 0;
};

} // namespace rail
