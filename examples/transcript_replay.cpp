// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
//
// One committed transcript, replayed against two chip graphs: proves the
// timing model answers to cache geometry, not just to what a program did.
// A single-line cache evicts on every one of the transcript's four distinct
// lines; a cache sized to hold all four does not, after its first pass.
// DRAM latency and hit latency are identical on both graphs, so geometry is
// the only thing that differs.
//
// examples/data/transcript.trace is a hand-written stand-in, in the
// core port event language timeball/core_port_trace.hpp reads. Issue #34
// replaces its contents with one converted from a real Spike commit log,
// unchanged in every other way: this file, and how it is read, do not change.

#include <cinttypes>
#include <cstdio>
#include <fstream>
#include <vector>

#include "timeball/core_port_trace.hpp"
#include "timeball/machine.hpp"
#include "timeball/timeball.hpp"

// NOLINTNEXTLINE(google-build-using-namespace)
using namespace timeball;

namespace {

constexpr Cycle kDramLatency = 100;
constexpr std::size_t kHitLatency = 2;

// A single 64-byte line, direct-mapped: every one of the transcript's four
// lines evicts the line before it.
using SingleLineCache = Machine<"L1", 1, 1, 64, LRUPolicy, kHitLatency>;
// Four 64-byte lines, one set each: the whole working set resident at once,
// so only the transcript's first pass over it misses.
using WorkingSetCache = Machine<"L1", 4, 1, 64, LRUPolicy, kHitLatency>;

template <typename Chip>
Cycle Replay(const std::vector<CorePortEvent>& events) {
  Chip machine({.dram_latency = kDramLatency});
  machine.GetCorePort().Apply(events);
  machine.GetCorePort().Sync();
  return machine.GetCorePort().Now();
}

}  // namespace

int main() {
  std::ifstream in(TIMEBALL_TRANSCRIPT_PATH);
  const auto events = ReadCorePortTrace(in);
  if (!events.has_value()) {
    std::fprintf(stderr, "malformed transcript: %s\n",
                 TIMEBALL_TRANSCRIPT_PATH);
    return 1;
  }

  const Cycle single_line = Replay<SingleLineCache>(*events);
  const Cycle working_set = Replay<WorkingSetCache>(*events);

  std::printf("single-line cache: %" PRIu64 " cycles\n", single_line.value());
  std::printf("working-set cache: %" PRIu64 " cycles\n", working_set.value());

  // The property this example exists to show — checked here, not just
  // asserted, so it still holds in a build where NDEBUG strips asserts.
  if (!(single_line > working_set)) {
    std::fprintf(stderr,
                 "a cache that thrashes on every access should finish later "
                 "than one that does not, but %" PRIu64
                 " is not greater than %" PRIu64 "\n",
                 single_line.value(), working_set.value());
    return 1;
  }
  return 0;
}
