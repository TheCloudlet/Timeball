// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_ADDRESS_MAP_HPP
#define TIMEBALL_ADDRESS_MAP_HPP

#include <cstdint>
#include <string_view>
#include <vector>

#include "timeball/node.hpp"

namespace timeball {

// Routes an access by address. Regions are half-open [base, end).
class AddressMap : public AccessNode {
  struct Region {
    uint64_t base = 0;
    uint64_t end = 0;  // exclusive
    AccessNode* node = nullptr;

    // The half-open rule lives here once, rather than being spelled out at
    // each place that needs it.
    [[nodiscard]] bool Contains(uint64_t addr) const {
      return addr >= base && addr < end;
    }
    [[nodiscard]] bool Overlaps(uint64_t other_base, uint64_t other_end) const {
      return other_base < end && base < other_end;
    }
  };

  // Linear scan. A chip has a few regions; replace this if a profile says so.
  std::vector<Region> regions_;

 public:
  // Configuration. Overlap, an empty range, or a null node asserts.
  void Map(uint64_t base, uint64_t end, AccessNode* node);

  // Whether addr is mapped. Ask this in a release build, where Map's asserts
  // are compiled out, before running.
  [[nodiscard]] bool Covers(uint64_t addr) const;

  [[nodiscard]] std::string_view NodeName() const override {
    return "AddressMap";
  }

  // Routing costs nothing: the access continues, untouched, to the node that
  // models its address. An unmapped address is a configuration error too: the
  // caller cannot handle it meaningfully, and returning a status would put a
  // branch on every access for a condition that must never happen. A release
  // build that needs to reject one asks Covers() first.
  Route Serve(const Request& r) override;
};

}  // namespace timeball

#endif  // TIMEBALL_ADDRESS_MAP_HPP
