// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/dma_device.hpp"

namespace timeball {

Route DmaDevice::Serve(const Request& r) {
  if (r.type == AccessType::kStore) {
    if (r.addr == 0) {
      source_ = r.value;
    } else if (r.addr == 8) {
      destination_ = r.value;
    } else if (r.addr == 16) {
      return {.cost = register_cycles_,
              .work = WorkDescription{
                  .origin = origin_,
                  .entry = entry_,
                  .issue_cycles = issue_cycles_,
                  .accesses = {
                      {.addr = source_, .type = AccessType::kLoad},
                      {.addr = destination_, .type = AccessType::kStore}}}};
    }
  }
  return {.cost = register_cycles_,
          .wait_for_work = r.type == AccessType::kLoad && r.addr == 24};
}

}  // namespace timeball
