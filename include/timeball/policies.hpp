// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_POLICIES_HPP
#define TIMEBALL_POLICIES_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>

namespace timeball {

// Replacement policies. Cache holds Policy::State<Sets, Ways>, so the state is
// a fixed-size array and hot-path bounds are constants.
//
// A new policy is a type with a nested State template providing:
//
//   struct MyPolicy {
//     template <size_t Sets, size_t Ways>
//     class State {
//      public:
//       void OnHit(size_t set_idx, size_t way_idx);   // a lookup hit this way
//       void OnFill(size_t set_idx, size_t way_idx);  // a line was placed here
//       size_t GetVictim(size_t set_idx);             // victim; may be const
//     };
//   };
//
// State is default-constructed, and GetVictim is called only when every way in
// the set is valid.

// 1. Least Recently Used (LRU)
struct LRUPolicy {
  template <size_t Sets, size_t Ways>
  class State {
    // [Set0_Way0, Set0_Way1, ... | Set1_Way0, ...]
    std::array<uint64_t, Sets * Ways> timestamps_{};

    std::array<uint64_t, Sets> set_counters_{};

   public:
    void OnHit(size_t set_idx, size_t way_idx) noexcept {
      timestamps_[(set_idx * Ways) + way_idx] = ++set_counters_[set_idx];
    }

    void OnFill(size_t set_idx, size_t way_idx) noexcept {
      OnHit(set_idx, way_idx);
    }

    [[nodiscard]] size_t GetVictim(size_t set_idx) const noexcept {
      size_t victim_way = 0;
      uint64_t min_time = std::numeric_limits<uint64_t>::max();

      const size_t base_idx = set_idx * Ways;

      // Linear scan of all ways in this set
      // Contiguous memory layout enables efficient CPU prefetching
      for (size_t way = 0; way < Ways; ++way) {
        if (timestamps_[base_idx + way] < min_time) {
          min_time = timestamps_[base_idx + way];
          victim_way = way;
        }
      }
      return victim_way;
    }
  };
};

// 2. First-In, First-Out (FIFO)
struct FIFOPolicy {
  template <size_t Sets, size_t Ways>
  class State {
    std::array<size_t, Sets> next_victim_{};  // Circular buffer index per set

   public:
    // Matches LRUPolicy's (set_idx, way_idx) order; single call site.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void OnHit(size_t set_idx, size_t way_idx) {
      // FIFO ignores hits
      (void)set_idx;
      (void)way_idx;
    }

    // Matches LRUPolicy's (set_idx, way_idx) order; single call site.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void OnFill(size_t set_idx, size_t way_idx) {
      (void)way_idx;
      // Increment circular buffer
      next_victim_[set_idx] = (next_victim_[set_idx] + 1) % Ways;
    }

    [[nodiscard]] size_t GetVictim(size_t set_idx) const {
      return next_victim_[set_idx];
    }
  };
};

// 3. Random Policy
//
// Same trace, same victims. The seed is fixed with the policy; another sequence
// is another build.
template <std::uint32_t Seed>
struct SeededRandomPolicy {
  template <size_t Sets, size_t Ways>
  class State {
    std::mt19937 search_rng_{Seed};

   public:
    // Unused. The signature matches LRUPolicy so a cache can call either.
    // NOLINTBEGIN(bugprone-easily-swappable-parameters)
    void OnHit(size_t set_idx, size_t way_idx) {
      (void)set_idx;
      (void)way_idx;
    }
    void OnFill(size_t set_idx, size_t way_idx) {
      (void)set_idx;
      (void)way_idx;
    }
    // NOLINTEND(bugprone-easily-swappable-parameters)

    // Not const: drawing a victim advances the generator.
    [[nodiscard]] size_t GetVictim(size_t set_idx) {
      (void)set_idx;
      std::uniform_int_distribution<size_t> dist(0, Ways - 1);
      return dist(search_rng_);
    }
  };
};

// std::mt19937's own default seed, so the name used everywhere a seed was never
// a question stays a plain type.
using RandomPolicy = SeededRandomPolicy<std::mt19937::default_seed>;

}  // namespace timeball

#endif  // TIMEBALL_POLICIES_HPP
