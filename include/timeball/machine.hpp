// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef TIMEBALL_MACHINE_HPP
#define TIMEBALL_MACHINE_HPP

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "timeball/cache.hpp"
#include "timeball/core_port.hpp"
#include "timeball/event_engine.hpp"
#include "timeball/memory.hpp"
#include "timeball/names.hpp"
#include "timeball/node.hpp"
#include "timeball/policies.hpp"

// One cache in front of one DRAM, plus an EventEngine and a CorePort. Geometry
// is the template arguments. DRAM latency and core port costs are
// MachineConfig. Devices are added with Attach. For several regions, pass an
// AddressMap's entry node to the second constructor. MemoryEntry() is only
// valid for the first.

namespace timeball {

struct MachineConfig {
  Cycle dram_latency = 100;
  CorePortConfig core_port;
};

template <FixedString CacheName, std::size_t Sets, std::size_t Ways,
          std::size_t BlockSize, typename ReplacePolicy = LRUPolicy,
          std::size_t HitLatency = 1>
class Machine {
 public:
  // The common case: one cache in front of one DRAM, both owned here.
  explicit Machine(MachineConfig config = {})
      : dram_(std::in_place, config.dram_latency),
        cache_(std::in_place, &*dram_),
        core_port_(engine_, *cache_, config.core_port) {}

  // Host-supplied memory graph. This Machine owns the engine and the core_port,
  // not the memory. memory must outlive it. The template geometry is unused.
  explicit Machine(AccessNode& memory, CorePortConfig config = {})
      : core_port_(engine_, memory, config) {}

  // A device's MMIO window, [base, base + size). The device must outlive the
  // Machine, as CorePort::Attach already requires.
  void Attach(uint64_t base, uint64_t size, MmioDevice& device) {
    core_port_.Attach(base, size, device);
  }

  [[nodiscard]] CorePort& GetCorePort() { return core_port_; }
  [[nodiscard]] EventEngine& Engine() { return engine_; }
  // The cache in front of DRAM, which is the entry of this machine's graph.
  // Asserts if this Machine was given a host-supplied topology; that host
  // already has the entry node.
  [[nodiscard]] AccessNode& MemoryEntry() {
    assert(cache_.has_value() &&
           "MemoryEntry() has no node to return: this Machine was constructed "
           "with a host-supplied memory topology instead of one of its own");
    return *cache_;
  }

 private:
  // Empty (and costing nothing beyond a flag each) when the host supplied its
  // own memory topology instead.
  std::optional<Memory<"DRAM">> dram_;
  std::optional<
      Cache<CacheName, Sets, Ways, BlockSize, ReplacePolicy, HitLatency>>
      cache_;
  EventEngine engine_;
  CorePort core_port_;
};

}  // namespace timeball

#endif  // TIMEBALL_MACHINE_HPP
