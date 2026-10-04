// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_MEMORY_HPP
#define TIMEBALL_MEMORY_HPP

#include <cstddef>
#include <string_view>

#include "timeball/names.hpp"
#include "timeball/node.hpp"

namespace timeball {

// Fixed-latency memory. Always served here: no tags, no misses, no forward.
// A DRAM and a scratchpad are two of these. The name, the latency, and the
// wiring differ. A second class does not.
template <FixedString Name = "Memory">
class Memory : public AccessNode {
  Cycle latency_;

 public:
  explicit Memory(Cycle lat = 100) : latency_(lat) {}

  [[nodiscard]] std::string_view NodeName() const override {
    return Name.value;
  }

  // Always hits. Held for the latency, so a later access waits.
  Route Serve(const Request& r) override {
    (void)r;
    return {.cost = latency_};
  }
};

}  // namespace timeball

#endif  // TIMEBALL_MEMORY_HPP
