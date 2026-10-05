// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/command_device.hpp"

#include <cassert>
#include <utility>

namespace timeball {

CommandDevice::CommandDevice(std::string_view name, uint64_t base,
                             Cycle access_cycles, std::size_t params,
                             uint64_t start_offset, uint64_t status_offset,
                             Cost cost)
    : name_(name),
      base_(base),
      access_cycles_(access_cycles),
      params_(params, 0),
      start_(start_offset),
      status_(status_offset),
      cost_(std::move(cost)) {}

Route CommandDevice::Serve(const Request& r) {
  assert(r.addr >= base_ && "Device access precedes its base address");
  const uint64_t offset = r.addr - base_;
  if (r.type == AccessType::kStore) {
    if (offset == start_) {
      return {.cost = access_cycles_, .work = cost_(params_)};
    }
    if (offset % 8 == 0 && offset / 8 < params_.size()) {
      params_[offset / 8] = r.value;
    }
  }
  return {.cost = access_cycles_,
          .wait_for_work = r.type == AccessType::kLoad && offset == status_};
}

}  // namespace timeball
