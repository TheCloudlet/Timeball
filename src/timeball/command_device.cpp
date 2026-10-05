// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/command_device.hpp"

#include <utility>

namespace timeball {

CommandDevice::CommandDevice(std::string_view name, InitiatorId origin,
                             Cycle access_cycles, std::size_t params,
                             uint64_t start_offset, uint64_t status_offset,
                             Cost cost)
    : name_(name),
      origin_(origin),
      access_cycles_(access_cycles),
      params_(params, 0),
      start_(start_offset),
      status_(status_offset),
      cost_(std::move(cost)) {}

Route CommandDevice::Serve(const Request& r) {
  if (r.type == AccessType::kStore) {
    if (r.addr == start_) {
      const Cycle cycles = cost_(params_);
      if (cycles > 0) {
        return {
            .cost = access_cycles_,
            .work = WorkDescription{.origin = origin_, .fixed_cycles = cycles}};
      }
      return {.cost = access_cycles_};
    }
    if (r.addr % 8 == 0 && r.addr / 8 < params_.size()) {
      params_[r.addr / 8] = r.value;
    }
  }
  return {.cost = access_cycles_,
          .wait_for_work = r.type == AccessType::kLoad && r.addr == status_};
}

}  // namespace timeball
