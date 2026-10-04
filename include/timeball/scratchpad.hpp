// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_SCRATCHPAD_HPP
#define TIMEBALL_SCRATCHPAD_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "timeball/names.hpp"
#include "timeball/node.hpp"

namespace timeball {

// Software-managed memory. Fixed latency, no tags, no misses. Any address that
// arrives is served. Not a base of MainMemory: this is not backing DRAM.
template <FixedString Name>
class Scratchpad : public AccessNode {
  Cycle latency_;

 public:
  explicit Scratchpad(Cycle lat) : latency_(lat) {}

  [[nodiscard]] std::string_view NodeName() const override {
    return Name.value;
  }

  // Served here, at the fixed cost. Held for that time, so a second arrival
  // waits.
  Route Serve(const Request& r) override {
    (void)r;
    return {.cost = latency_};
  }
};

}  // namespace timeball

#endif  // TIMEBALL_SCRATCHPAD_HPP
