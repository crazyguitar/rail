#include "rail/transport/rdma-device.h"

#include <mutex>
#include <unordered_map>

namespace rail {

namespace {

struct Registry {
  std::mutex Lock;
  std::unordered_map<std::string, std::weak_ptr<RdmaDevice>> Live;
  // Never shrinks, so a device closed and reopened keeps the slot every
  // region's registration table already indexes it by.
  std::unordered_map<std::string, size_t> Slots;

  static Registry &get() {
    static Registry Only;
    return Only;
  }
};

} // namespace

RdmaDevice::~RdmaDevice() {
  if (Domain) ibv_dealloc_pd(Domain);
  if (Context) ibv_close_device(Context);
}

Result<std::shared_ptr<RdmaDevice>> RdmaDevice::open(const std::string &Name) {
  Registry &R = Registry::get();
  const std::lock_guard<std::mutex> Held(R.Lock);

  if (auto It = R.Live.find(Name); It != R.Live.end())
    if (auto Already = It->second.lock()) return Already;

  int Count = 0;
  ibv_device **List = ibv_get_device_list(&Count);
  if (!List) return failErrno("ibv_get_device_list");

  ibv_context *Context = nullptr;
  for (int I = 0; I < Count && !Context; I++)
    if (Name == ibv_get_device_name(List[I])) Context = ibv_open_device(List[I]);
  ibv_free_device_list(List);
  if (!Context) return failMessage("no rdma device named " + Name);

  ibv_pd *Domain = ibv_alloc_pd(Context);
  if (!Domain) {
    ibv_close_device(Context);
    return failMessage("ibv_alloc_pd failed for " + Name);
  }

  auto Slot = R.Slots.find(Name);
  if (Slot == R.Slots.end()) Slot = R.Slots.emplace(Name, R.Slots.size()).first;

  std::shared_ptr<RdmaDevice> Made(new RdmaDevice(Name, Context, Domain, Slot->second));
  R.Live[Name] = Made;
  return Made;
}

} // namespace rail
