// Copyright 2025-2026 Yi-Ping Pan (Cloudlet)
//
// How fast the engine times accesses, and how much it holds while doing so.
//
//   throughput [hits|misses|agents]    one scenario per process, so the peak
//                                      resident size is that scenario's own
//
// Each scenario times 2,000,000 accesses through L1 -> L2 -> DRAM, submitted
// in windows of 10,000 cycles and run window by window, as a long-running host
// would. Output is one line: scenario, accesses per second, the engine's peak
// in-flight state, and the process's peak resident size.
//
// Deliberately timed without timeball/checking_sink.hpp attached: every record
// costs it a map lookup, and that cost would land inside the number this file
// exists to measure. Its correctness is covered elsewhere — the same
// scheduling code path runs, checked, in test_reference_scheduler.cpp and in
// the CheckingSink-attached tests, and this file's own record stream is
// compared record-for-record against a prior commit's.

#include <sys/resource.h>

#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "timeball/timeball.hpp"

// Deliberate, as in the example: every name here is timeball's.
// NOLINTNEXTLINE(google-build-using-namespace)
using namespace timeball;

namespace {

constexpr uint64_t kAccesses = 2'000'000;
constexpr Cycle kWindow = 10'000;
// Cycles between one agent's accesses: spaced so the memory keeps up. A
// saturated memory would measure a queue growing without bound, not the
// engine.
constexpr Cycle kHitSpacing = 20;
constexpr Cycle kMissSpacing = 150;

struct Hierarchy {
  MainMemory<"DRAM"> dram{100};
  Cache<"L2", 512, 8, 64, LRUPolicy, 10> l2{&dram};
  Cache<"L1a", 64, 8, 64, LRUPolicy, 4> l1a{&l2};
  Cache<"L1b", 64, 8, 64, LRUPolicy, 4> l1b{&l2};
  Cache<"L1c", 64, 8, 64, LRUPolicy, 4> l1c{&l2};
};

// A cheap deterministic stream of addresses.
uint64_t Next(uint64_t& x) {
  x = (x * 6364136223846793005ULL) + 1442695040888963407ULL;
  return x >> 20;
}

int64_t PeakResidentKb() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
#ifdef __APPLE__
  return usage.ru_maxrss / 1024;  // bytes on macOS
#else
  return usage.ru_maxrss;  // kilobytes on Linux
#endif
}

}  // namespace

int main(int argc, char** argv) {
  const char* scenario = argc > 1 ? argv[1] : "hits";
  const bool misses = std::strcmp(scenario, "misses") == 0;
  const int agents = std::strcmp(scenario, "agents") == 0 ? 3 : 1;

  auto h = std::make_unique<Hierarchy>();  // megabytes of lines: not the stack
  AccessNode* entry[] = {&h->l1a, &h->l1b, &h->l1c};
  EventEngine engine;
  uint64_t x = 1;
  std::size_t peak = 0;

  const auto begin = std::chrono::steady_clock::now();
  uint64_t issued = 0;
  for (Cycle window = 0; issued < kAccesses; window += kWindow) {
    for (Cycle t = window; t < window + kWindow && issued < kAccesses;
         t += misses ? kMissSpacing : kHitSpacing) {
      for (int a = 0; a < agents && issued < kAccesses; ++a, ++issued) {
        // Hits: a 16 KB working set per agent, which L1 holds. Misses: 4 GB.
        const uint64_t addr = misses ? (Next(x) % (1ULL << 26)) * 64
                                     : (Next(x) % 256) * 64 + (a << 20);
        engine.SubmitAccess(*entry[a],
                            {.addr = addr,
                             .type = AccessType::kLoad,
                             .initiator_id = static_cast<uint32_t>(a)},
                            {.ready_cycle = t});
      }
    }
    engine.RunUntil(window + kWindow);
    peak = std::max(peak, engine.InFlight());
  }
  engine.RunUntilIdle();
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - begin)
          .count();

  std::printf("%-7s %6.2f M accesses/s  peak in-flight %8zu  peak RSS %6" PRId64
              " MB\n",
              scenario, static_cast<double>(kAccesses) / seconds / 1e6, peak,
              PeakResidentKb() / 1024);
  return 0;
}
