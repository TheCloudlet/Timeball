// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_DMA_DEVICE_HPP
#define TIMEBALL_DMA_DEVICE_HPP

#include <cstdint>
#include <string_view>

#include "timeball/node.hpp"

namespace timeball {

// Copies one access unit: source at +0, destination at +8, START at +16,
// STATUS at +24. Only timing is modeled; the host owns the data. A STATUS
// read waits for the copy while the registers remain available.
class DmaDevice final : public AccessNode {
 public:
  // name, entry, and this device must outlive its in-flight work. entry is
  // where this DMA's accesses enter, often below the core's private L1.
  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
  DmaDevice(std::string_view name, uint64_t base, InitiatorId origin,
            AccessNode& entry, Cycle register_cycles, Cycle issue_cycles)
      : name_(name),
        base_(base),
        origin_(origin),
        entry_(&entry),
        register_cycles_(register_cycles),
        issue_cycles_(issue_cycles) {}

  [[nodiscard]] std::string_view NodeName() const override { return name_; }
  Route Serve(const Request& r) override;

 private:
  std::string_view name_;
  uint64_t base_;
  InitiatorId origin_;
  AccessNode* entry_;
  Cycle register_cycles_;
  Cycle issue_cycles_;
  uint64_t source_ = 0;
  uint64_t destination_ = 0;
};

}  // namespace timeball

#endif  // TIMEBALL_DMA_DEVICE_HPP
