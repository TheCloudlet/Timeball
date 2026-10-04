// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#include "timeball/address_map.hpp"

#include <algorithm>
#include <cassert>
#include <cstdint>

namespace timeball {

namespace {

// Where an unmapped access goes when asserts are compiled out. It costs the
// rest of the timeline, so the bad access reads as never finishing rather than
// as a fast success. It is a node of its own so that only it is held for that
// long: the map, and every mapped node behind it, stay free for everyone else.
class Unmapped final : public AccessNode {
 public:
  [[nodiscard]] std::string_view NodeName() const override {
    return "<unmapped>";
  }
  Route Serve(const Request& r) override {
    (void)r;
    return {.cost = UINT64_MAX};
  }
};

}  // namespace

void AddressMap::Map(uint64_t base, uint64_t end, AccessNode* node) {
  assert(node != nullptr && "Mapped region has no node");
  assert(base < end && "Mapped region is empty or inverted");
  for (const auto& r : regions_) {
    assert(!r.Overlaps(base, end) && "Mapped regions overlap");
  }
  regions_.push_back({.base = base, .end = end, .node = node});
}

bool AddressMap::Covers(uint64_t addr) const {
  return std::ranges::any_of(
      regions_, [addr](const Region& region) { return region.Contains(addr); });
}

Route AddressMap::Serve(const Request& r) {
  for (const auto& region : regions_) {
    if (region.Contains(r.addr)) {
      // The address is passed through untouched: a node behind the map does its
      // own set/tag split on the address the host actually used.
      return {.cost = 0, .next = region.node, .forward = r};
    }
  }
  assert(false && "Access to an unmapped address");
  // Unreachable in a debug build, and unreachable by construction in a release
  // build that checked Covers() first. Stateless, so one serves every map.
  static Unmapped unmapped;
  return {.cost = 0, .next = &unmapped, .forward = r};
}

}  // namespace timeball
