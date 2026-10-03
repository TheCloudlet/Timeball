// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_MEMORY_HPP
#define TIMEBALL_MEMORY_HPP

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "timeball/names.hpp"
#include "timeball/node.hpp"

namespace timeball {

// Backing memory. Fixed latency, always hits, held for that service time.
template <FixedString Name = "MainMemory">
class MainMemory : public AccessNode {
  Cycle latency_;

 public:
  explicit MainMemory(Cycle lat = 100) : latency_(lat) {}

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
