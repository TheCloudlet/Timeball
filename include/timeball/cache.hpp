// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_CACHE_HPP
#define TIMEBALL_CACHE_HPP

#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "timeball/names.hpp"
#include "timeball/node.hpp"
#include "timeball/policies.hpp"

namespace timeball {

// Cache<"L1", Sets, Ways, BlockSize, Policy, HitLatency> in front of next.
// Geometry and policy are compile-time, so tag lookup is inlined. Only the
// successor is dynamic.
template <FixedString Name, size_t Sets, size_t Ways, size_t BlockSize,
          typename ReplacePolicy = LRUPolicy, size_t HitLatency = 1,
          bool InstructionCache = false>
class Cache : public AccessNode {
  // Sets and BlockSize index the address, so both must be powers of two for the
  // set/tag split to be a bitfield extract. Ways is only a loop bound and a
  // stride, so an odd associativity is legitimate and is not rejected.
  static_assert(Sets > 0 && (Sets & (Sets - 1)) == 0,
                "Sets must be a non-zero power of two");
  static_assert(BlockSize > 0 && (BlockSize & (BlockSize - 1)) == 0,
                "BlockSize must be a non-zero power of two");
  static_assert(Ways > 0, "Ways must be non-zero");

  struct Line {
    bool valid = false;
    bool dirty = false;
    uint64_t tag = 0;
  };

  // Not owned. Several caches may share one successor, which is why this is not
  // a template parameter. The successor must outlive this cache.
  AccessNode* next_;

  // [Set0_Way0, Set0_Way1, ... | Set1_Way0, ...]
  std::array<Line, Sets * Ways> sets_;

  typename ReplacePolicy::template State<Sets, Ways> policy_;

  // -------------------------------------------------------
  // |       Tag       |    Set Index    |  Block Offset   |
  // -------------------------------------------------------
  static size_t SetIndex(uint64_t addr) { return (addr / BlockSize) % Sets; }
  static uint64_t Tag(uint64_t addr) { return addr / (BlockSize * Sets); }

  // The way holding this address, or Ways if it is not resident.
  [[nodiscard]] size_t Find(uint64_t addr) const {
    const size_t set_idx = SetIndex(addr);
    const uint64_t tag = Tag(addr);
    const size_t base_idx = set_idx * Ways;
    for (size_t way_idx = 0; way_idx < Ways; ++way_idx) {
      const Line& line = sets_[base_idx + way_idx];
      if (line.valid && line.tag == tag) {
        return way_idx;
      }
    }
    return Ways;
  }

 public:
  // Null successor is a configuration error, so it asserts.
  explicit Cache(AccessNode* next) : next_(next) {
    assert(next_ != nullptr && "Cache constructed without a successor");
  }

  [[nodiscard]] std::string_view NodeName() const override {
    return Name.value;
  }

  // Held for the lookup only. A miss is not also held for the round trip, or
  // that time would be charged twice.
  Route Serve(const Request& r) override {
    if (r.type == AccessType::kFence) {
      // Instruction-cache invalidation must not discard dirty data-cache lines.
      if constexpr (InstructionCache) {
        sets_ = {};
        policy_ = {};
      }
      return {.cost = HitLatency};
    }
    const size_t set_idx = SetIndex(r.addr);
    const size_t way_idx = Find(r.addr);
    if (way_idx != Ways) {
      // Hit. The next level is not touched.
      if (r.type == AccessType::kStore) {
        sets_[(set_idx * Ways) + way_idx].dirty = true;
      }
      if (r.type != AccessType::kHint) {
        policy_.OnHit(set_idx, way_idx);
      }
      return {.cost = HitLatency};
    }

    // Miss. A write miss allocates, so the line is read in first.
    Request down = r;
    down.type = AccessType::kLoad;
    return {.cost = HitLatency, .next = next_, .forward = down};
  }

  // Loads and stores share this path. A store also marks the filled line dirty.
  std::vector<Writeback> Complete(const Request& r, bool forwarded) override {
    if (!forwarded) {
      return {};
    }
    // Fill now, not when the miss was noticed. Until then the line misses.
    const size_t set_idx = SetIndex(r.addr);
    std::vector<Writeback> writebacks;
    const size_t way = Fill(r.addr, r.initiator_id, writebacks);
    if (r.type == AccessType::kStore) {
      sets_[(set_idx * Ways) + way].dirty = true;
    }
    return writebacks;
  }

 private:
  // Returns the way filled, so a store can mark it dirty without a second scan.
  // A dirty victim's writeback is attributed to the initiator whose fill
  // evicted it.
  size_t Fill(uint64_t addr, InitiatorId initiator_id,
              std::vector<Writeback>& writebacks) {
    const size_t set_idx = SetIndex(addr);
    const uint64_t tag = Tag(addr);
    // A second miss to a line already filled by an in-flight miss reuses it.
    const size_t resident = Find(addr);
    if (resident != Ways) {
      policy_.OnHit(set_idx, resident);
      return resident;
    }

    size_t victim_way_idx = Ways;
    size_t base_idx = set_idx * Ways;

    for (size_t way_idx = 0; way_idx < Ways; ++way_idx) {
      if (!sets_[base_idx + way_idx].valid) {
        victim_way_idx = way_idx;
        break;
      }
    }

    if (victim_way_idx == Ways) {
      victim_way_idx = policy_.GetVictim(set_idx);
      size_t victim_flat_idx = base_idx + victim_way_idx;
      Line& victim = sets_[victim_flat_idx];
      if (victim.valid && victim.dirty) {
        uint64_t evict_addr = ((victim.tag * Sets) + set_idx) * BlockSize;
        // Holds the level below for its own service. Not on the requester's
        // path, but it allocates there, so dropping it would change later hits.
        writebacks.push_back({.to = next_,
                              .request = {.addr = evict_addr,
                                          .type = AccessType::kStore,
                                          .initiator_id = initiator_id}});
      }
    }

    // Fill the line
    size_t fill_idx = base_idx + victim_way_idx;
    sets_[fill_idx].valid = true;
    sets_[fill_idx].tag = tag;
    sets_[fill_idx].dirty = false;
    policy_.OnFill(set_idx, victim_way_idx);
    return victim_way_idx;
  }
};

}  // namespace timeball

#endif  // TIMEBALL_CACHE_HPP
