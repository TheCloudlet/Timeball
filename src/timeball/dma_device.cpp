// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/dma_device.hpp"

#include <cassert>

namespace timeball {

Route DmaDevice::Serve(const Request& r) {
  assert(r.addr >= base_ && "DMA access precedes its base address");
  const uint64_t offset = r.addr - base_;
  if (r.type == AccessType::kStore) {
    if (offset == 0) {
      source_ = r.value;
    } else if (offset == 8) {
      destination_ = r.value;
    } else if (offset == 16) {
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
          .wait_for_work = r.type == AccessType::kLoad && offset == 24};
}

}  // namespace timeball
