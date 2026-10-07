// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)

#ifndef EXAMPLES_SPIKE_MACHINES_HPP
#define EXAMPLES_SPIKE_MACHINES_HPP

#include "timeball/machine.hpp"

// Two machines for one transcript. DRAM latency and hit latency are the same;
// only the cache's geometry changes. The one-line cache holds a single 64-byte
// block. The other holds 64 KiB, which covers the data working set of
// examples/spike/scan.c, including a proxy kernel's own data accesses.

namespace spike_example {

inline constexpr timeball::MachineConfig kReplayConfig{
    .dram_latency = 100,
    .core_port = {.cpi = 1},
};

using OneLineMachine =
    timeball::Machine<"L1", 1, 1, 64, timeball::LRUPolicy, 1>;
using WorkingSetMachine =
    timeball::Machine<"L1", 256, 4, 64, timeball::LRUPolicy, 1>;

}  // namespace spike_example

#endif  // EXAMPLES_SPIKE_MACHINES_HPP
